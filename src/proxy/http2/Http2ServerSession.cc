/** @file

  Http2ServerSession.

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include "proxy/http2/Http2ServerSession.h"
#include "iocore/net/NetVConnection.h"
#include "iocore/net/TLSSNISupport.h"
#include "proxy/http/HttpDebugNames.h"
#include "tscore/ink_base64.h"
#include "proxy/http2/Http2CommonSessionInternal.h"
#include "proxy/http/HttpSessionManager.h"

ClassAllocator<Http2ServerSession> http2ServerSessionAllocator("http2ServerSessionAllocator");

static int
send_connection_event(Continuation *cont, int event, void *edata)
{
  SCOPED_MUTEX_LOCK(lock, cont->mutex, this_ethread());
  return cont->handleEvent(event, edata);
}

Http2ServerSession::Http2ServerSession() = default;

void
Http2ServerSession::destroy()
{
  if (!in_destroy) {
    in_destroy = true;
    write_vio  = nullptr;
    this->remove_session();
    this->release_outbound_connection_tracking();
    REMEMBER(NO_EVENT, this->recursion)
    Http2SsnDebug("session destroy");
    if (_vc) {
      _vc->do_io_close();
      _vc = nullptr;
    }
    free();
  }
}

void
Http2ServerSession::free()
{
  auto mutex_thread = this->mutex->thread_holding;
  if (Http2CommonSession::common_free(this)) {
    Metrics::Gauge::decrement(http2_rsb.current_server_session_count);
    THREAD_FREE(this, http2ServerSessionAllocator, mutex_thread);
  }
}

void
Http2ServerSession::start()
{
  SCOPED_MUTEX_LOCK(lock, this->mutex, this_ethread());

  SET_HANDLER(&Http2ServerSession::main_event_handler);
  HTTP2_SET_SESSION_HANDLER(&Http2ServerSession::state_start_frame_read);

  VIO *read_vio = this->do_io_read(this, INT64_MAX, this->read_buffer);
  write_vio     = this->do_io_write(this, INT64_MAX, this->_write_buffer_reader);

  this->connection_state.init(this);

  // 3.5 HTTP/2 Connection Preface. Upon establishment of a TCP connection and
  // determination that HTTP/2 will be used by both peers, each endpoint MUST
  // send a connection preface as a final confirmation ...
  // This is the preface string sent by the client
  this->write_buffer->write(HTTP2_CONNECTION_PREFACE, HTTP2_CONNECTION_PREFACE_LEN);
  write_reenable();
  this->connection_state.send_connection_preface();
  Http2SsnDebug("Sent Connection Preface");

  this->handleEvent(VC_EVENT_READ_READY, read_vio);
}

void
Http2ServerSession::new_connection(NetVConnection *new_vc, MIOBuffer *iobuf, IOBufferReader *reader)
{
  ink_assert(new_vc->mutex->thread_holding == this_ethread());

  Metrics::Gauge::increment(http2_rsb.current_server_session_count);
  Metrics::Counter::increment(http2_rsb.total_server_connection_count);
  this->_milestones.mark(Http2SsnMilestone::OPEN);

  // Unique client session identifier.
  this->con_id = ProxySession::next_connection_id();
  this->_vc    = new_vc;
  _vc->set_inactivity_timeout(HRTIME_SECONDS(Http2::accept_no_activity_timeout));
  this->schedule_event = nullptr;
  this->mutex          = new_vc->mutex;

  this->connection_state.mutex = this->mutex;

  // Since we're functioning as a client, we do not need to worry about
  // TLSEarlyDataSupport.

  Http2SsnDebug("session born, netvc %p", this->_vc);

  this->_vc->set_tcp_congestion_control(NetVConnection::tcp_congestion_control_side::CLIENT_SIDE);

  this->read_buffer             = iobuf ? iobuf : new_MIOBuffer(HTTP2_HEADER_BUFFER_SIZE_INDEX);
  this->read_buffer->water_mark = connection_state.local_settings.get(HTTP2_SETTINGS_MAX_FRAME_SIZE);
  this->_read_buffer_reader     = reader ? reader : this->read_buffer->alloc_reader();

  // Set write buffer size to max size of TLS record (16KB)
  // This block size is the buffer size that we pass to SSLWriteBuffer
  auto buffer_block_size_index = iobuffer_size_to_index(Http2::write_buffer_block_size, MAX_BUFFER_SIZE_INDEX);
  this->write_buffer           = new_MIOBuffer(buffer_block_size_index);
  this->_write_buffer_reader   = this->write_buffer->alloc_reader();
  this->_write_size_threshold  = index_to_buffer_size(buffer_block_size_index) * Http2::write_size_threshold;

  uint32_t buffer_water_mark;
  if (auto snis = this->_vc->get_service<TLSSNISupport>(); snis && snis->hints_from_sni.http2_buffer_water_mark.has_value()) {
    buffer_water_mark = snis->hints_from_sni.http2_buffer_water_mark.value();
  } else {
    buffer_water_mark = Http2::buffer_water_mark;
  }
  this->write_buffer->water_mark = buffer_water_mark;

  this->_handle_if_ssl(new_vc);

  do_api_callout(TS_HTTP_SSN_START_HOOK);

  this->add_session();
}

// implement that. After we send a GOAWAY, there
// are scenarios where we would like to complete the outstanding streams.

void
Http2ServerSession::do_io_close(int /* alerrno ATS_UNUSED */)
{
  REMEMBER(NO_EVENT, this->recursion)

  if (!this->connection_state.is_state_closed()) {
    Http2SsnDebug("session closed");
    this->remove_session();

    ink_assert(this->mutex->thread_holding == this_ethread());
    send_connection_event(&this->connection_state, HTTP2_SESSION_EVENT_FINI, this);

    // Destroy will be called from connection_state.release_stream() once the number of active streams goes to 0
  }
}

int
Http2ServerSession::main_event_handler(int event, void *edata)
{
  ink_assert(this->mutex->thread_holding == this_ethread());
  int retval;

  recursion++;

  Event *e = static_cast<Event *>(edata);
  if (e == schedule_event) {
    schedule_event = nullptr;
  }

  Http2SsnDebug("main_event_handler=%d edata=%p", event, edata);

  switch (event) {
  case VC_EVENT_READ_COMPLETE:
  case VC_EVENT_READ_READY: {
    bool is_zombie = connection_state.get_zombie_event() != nullptr;
    retval         = (this->*session_handler)(event, edata);
    if (is_zombie && connection_state.get_zombie_event() != nullptr) {
      Warning("Processed read event for zombie session %" PRId64, connection_id());
    }
    break;
  }

  case HTTP2_SESSION_EVENT_REENABLE:
    // VIO will be reenableed in this handler
    retval = (this->*session_handler)(VC_EVENT_READ_READY, static_cast<VIO *>(e->cookie));
    // Clear the event after calling session_handler to not reschedule REENABLE in it
    this->_reenable_event = nullptr;
    break;

  case VC_EVENT_ACTIVE_TIMEOUT:
  case VC_EVENT_INACTIVITY_TIMEOUT:
  case VC_EVENT_ERROR:
  case VC_EVENT_EOS:
    Http2SsnDebug("Closing event: %s", HttpDebugNames::get_event_name(event));
    this->set_dying_event(event);
    this->do_io_close();
    retval = 0;
    break;

  case VC_EVENT_WRITE_READY:
  case VC_EVENT_WRITE_COMPLETE:
    this->connection_state.restart_streams();
    if ((ink_get_hrtime() >= this->_write_buffer_last_flush + HRTIME_MSECONDS(this->_write_time_threshold))) {
      this->flush();
    }

    retval = 0;
    break;

  case HTTP2_SESSION_EVENT_PRIO:
  default:
    Http2SsnDebug("unexpected event=%d edata=%p", event, edata);
    ink_release_assert(0);
    retval = 0;
    break;
  }

  if (!this->is_draining() && this->connection_state.get_shutdown_reason() == Http2ErrorCode::HTTP2_ERROR_MAX) {
    this->connection_state.set_shutdown_state(HTTP2_SHUTDOWN_NONE);
  }

  if (this->connection_state.get_shutdown_state() == HTTP2_SHUTDOWN_NONE) {
    if (this->is_draining()) { // For a case we already checked Connection header and it didn't exist
      Http2SsnDebug("Preparing for graceful shutdown because of draining state");
      this->connection_state.set_shutdown_state(HTTP2_SHUTDOWN_NOT_INITIATED);
    } /*else if (this->connection_state.get_stream_error_rate() >
               Http2::stream_error_rate_threshold) { // For a case many stream errors happened
      ip_port_text_buffer ipb;
      const char *client_ip = ats_ip_ntop(get_remote_addr(), ipb, sizeof(ipb));
      SiteThrottledWarning("HTTP/2 session error origin_ip=%s session_id=%" PRId64
              " closing a connection, because its stream error rate (%f) exceeded the threshold (%f)",
              client_ip, connection_id(), this->connection_state.get_stream_error_rate(), Http2::stream_error_rate_threshold);
      Http2SsnDebug("Preparing for graceful shutdown because of a high stream error rate");
      cause_of_death = Http2SessionCod::HIGH_ERROR_RATE;
      this->connection_state.set_shutdown_state(HTTP2_SHUTDOWN_NOT_INITIATED, Http2ErrorCode::HTTP2_ERROR_ENHANCE_YOUR_CALM);
    } */
  }

  if (this->connection_state.get_shutdown_state() == HTTP2_SHUTDOWN_NOT_INITIATED) {
    send_connection_event(&this->connection_state, HTTP2_SESSION_EVENT_SHUTDOWN_INIT, this);
  }

  recursion--;
  if (!connection_state.is_recursing() && this->recursion == 0 && kill_me) {
    this->free();
  }
  return retval;
}

sockaddr const *
Http2ServerSession::get_remote_addr() const
{
  return _vc ? _vc->get_remote_addr() : &cached_client_addr.sa;
}

sockaddr const *
Http2ServerSession::get_local_addr()
{
  return _vc ? _vc->get_local_addr() : &cached_local_addr.sa;
}

int
Http2ServerSession::get_transact_count() const
{
  return connection_state.get_stream_requests();
}

const char *
Http2ServerSession::get_protocol_string() const
{
  return "http/2";
}

void
Http2ServerSession::release(ProxyTransaction * /* trans ATS_UNUSED */)
{
}

int
Http2ServerSession::populate_protocol(std::string_view *result, int size) const
{
  int retval = 0;
  if (size > retval) {
    result[retval++] = IP_PROTO_TAG_HTTP_2_0;
    if (size > retval) {
      retval += super::populate_protocol(result + retval, size - retval);
    }
  }
  return retval;
}

const char *
Http2ServerSession::protocol_contains(std::string_view prefix) const
{
  const char *retval = nullptr;

  if (prefix.size() <= IP_PROTO_TAG_HTTP_2_0.size() && strncmp(IP_PROTO_TAG_HTTP_2_0.data(), prefix.data(), prefix.size()) == 0) {
    retval = IP_PROTO_TAG_HTTP_2_0.data();
  } else {
    retval = super::protocol_contains(prefix);
  }
  return retval;
}

ProxySession *
Http2ServerSession::get_proxy_session()
{
  return this;
}

ProxyTransaction *
Http2ServerSession::new_transaction()
{
  this->set_session_active();

  // Create a new stream/transaction
  Http2Error   error(Http2ErrorClass::HTTP2_ERROR_CLASS_NONE);
  Http2Stream *stream = connection_state.create_initiating_stream(error);

  if (!stream || connection_state.is_peer_concurrent_stream_ub()) {
    if (error.cls != Http2ErrorClass::HTTP2_ERROR_CLASS_NONE) {
      Error("HTTP/2 stream error code=0x%02x %s", static_cast<int>(error.code), error.msg);
    }

    remove_session();
  }

  return stream;
}

void
Http2ServerSession::add_session()
{
  if (this->in_session_table) {
    return;
  }
  Http2SsnDebug("Add session to pool");
  EThread           *ethread = this_ethread();
  ServerSessionPool *pool    = ethread->server_session_pool;
  MUTEX_TRY_LOCK(lock, pool->mutex, ethread);
  if (lock.is_locked()) {
    pool->addSession(this);
    this->in_session_table = true;
  }
}

void
Http2ServerSession::remove_session()
{
  if (!this->in_session_table) {
    return;
  }
  Http2SsnDebug("Remove session from pool");
  EThread           *ethread = this_ethread();
  ServerSessionPool *pool    = ethread->server_session_pool;
  MUTEX_TRY_LOCK(lock, pool->mutex, ethread);
  if (lock.is_locked()) {
    pool->removeSession(this);
    in_session_table = false;
  } else {
    ink_release_assert(!"How did we not get the pool lock?");
  }
}

bool
Http2ServerSession::is_multiplexing() const
{
  return true;
}

bool
Http2ServerSession::is_outbound() const
{
  return true;
}

void
Http2ServerSession::set_netvc(NetVConnection *netvc)
{
  super::set_netvc(netvc);
  if (netvc == nullptr) {
    write_vio = nullptr;
  }
}

void
Http2ServerSession::set_no_activity_timeout()
{
  // Only set if not previously set
  if (this->_vc->get_inactivity_timeout() == 0) {
    this->set_inactivity_timeout(HRTIME_SECONDS(Http2::no_activity_timeout_out));
  }
}

HTTPVersion
Http2ServerSession::get_version(HTTPHdr & /* hdr ATS_UNUSED */) const
{
  return HTTP_2_0;
}

IOBufferReader *
Http2ServerSession::get_remote_reader()
{
  return _read_buffer_reader;
}

bool
Http2ServerSession::is_protocol_framed() const
{
  return true;
}

uint64_t
Http2ServerSession::get_received_frame_count(uint64_t type) const
{
  if (type == 999) { // TS_SSN_INFO_RECEIVED_FRAME_COUNT_H2_UNKNOWN in apidefs.h.in
    return this->_frame_counts_in[HTTP2_FRAME_TYPE_MAX];
  } else {
    return this->_frame_counts_in[type];
  }
}

std::function<PoolableSession *()> create_h2_server_session = []() -> PoolableSession * {
  return http2ServerSessionAllocator.alloc();
};
