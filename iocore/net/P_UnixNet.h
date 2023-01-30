/** @file

  A brief file description

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

#pragma once

#include <bitset>

#include "tscore/ink_platform.h"
#include "I_EThread.h"
#include "I_Continuation.h"
#include "I_IOStrategy.h"
#include "P_Net.h"
#include "P_EventIO.h"
#include "P_UnixNetProcessor.h"
#include "P_UnixNetVConnection.h"
#include "P_NetAccept.h"
#include "P_DNSConnection.h"
#include "P_UnixUDPConnection.h"
#include "P_UnixPollDescriptor.h"
#include <limits>

struct PollDescriptor;

class NetEvent;
class NetHandler;
using NetContHandler = int (NetHandler::*)(int, void *);

extern ink_hrtime last_throttle_warning;
extern ink_hrtime last_shedding_warning;
extern ink_hrtime emergency_throttle_time;
extern int net_connections_throttle;
extern bool net_memory_throttle;
extern int fds_throttle;
extern int fds_limit;
extern ink_hrtime last_transient_accept_error;
extern int http_accept_port_number;

//
// Configuration Parameter had to move here to share
// between UnixNet and UnixUDPNet or SSLNet modules.
// Design notes are in Memo.NetDesign
//

#define THROTTLE_FD_HEADROOM (128 + 64) // CACHE_DB_FDS + 64

#define TRANSIENT_ACCEPT_ERROR_MESSAGE_EVERY HRTIME_HOURS(24)

// also the 'throttle connect headroom'
#define EMERGENCY_THROTTLE       16
#define THROTTLE_AT_ONCE         5
#define HYPER_EMERGENCY_THROTTLE 6

#define NET_THROTTLE_ACCEPT_HEADROOM  1.1 // 10%
#define NET_THROTTLE_CONNECT_HEADROOM 1.0 // 0%
#define NET_THROTTLE_MESSAGE_EVERY    HRTIME_MINUTES(10)

#define PRINT_IP(x) ((uint8_t *)&(x))[0], ((uint8_t *)&(x))[1], ((uint8_t *)&(x))[2], ((uint8_t *)&(x))[3]

// function prototype needed for SSLUnixNetVConnection
unsigned int net_next_connection_number();

/**
  NetHandler is the processor of NetEvent for the Net sub-system. The NetHandler
  is the core component of the Net sub-system. Once started, it is responsible
  for polling socket fds and perform the I/O tasks in NetEvent.

  The NetHandler is executed periodically to perform read/write tasks for
  NetVConnection. The NetHandler::mainNetEvent() should be viewed as a part of
  EThread::execute() loop. This is the reason that Net System is a sub-system.

  By get_NetHandler(this_ethread()), you can get the NetHandler object that
  runs inside the current EThread and then @c startIO / @c stopIO which
  assign/release a NetEvent to/from NetHandler. Before you call these functions,
  holding the mutex of this NetHandler is required.

  The NetVConnection provides a set of do_io functions through which you can
  specify continuations to be called back by its NetHandler. These function
  calls do not block. Instead they return an VIO object and schedule the
  callback to the continuation passed in when there are I/O events occurred.

  Multi-thread scheduler:

  The NetHandler should be viewed as multi-threaded schedulers which process
  NetEvents from their queues. If vc wants to be managed by NetHandler, the vc
  should be derived from NetEvent. The vc can be made of NetProcessor (allocate_vc)
  either by directly adding a NetEvent to the queue (NetHandler::startIO), or more
  conveniently, calling a method service call (NetProcessor::connect_re) which
  synthesizes the NetEvent and places it in the queue.

  Callback event codes:

  These event codes for do_io_read and reenable(read VIO) task:
    VC_EVENT_READ_READY, VC_EVENT_READ_COMPLETE,
    VC_EVENT_EOS, VC_EVENT_ERROR

  These event codes for do_io_write and reenable(write VIO) task:
    VC_EVENT_WRITE_READY, VC_EVENT_WRITE_COMPLETE
    VC_EVENT_ERROR

  There is no event and callback for do_io_shutdown / do_io_close task.

  NetVConnection allocation policy:

  VCs are allocated by the NetProcessor and deallocated by NetHandler.
  A state machine may access the returned, non-recurring NetEvent / VIO until
  it is closed by do_io_close. For recurring NetEvent, the NetEvent may be
  accessed until it is closed. Once the NetEvent is closed, it's the
  NetHandler's responsibility to deallocate it.

  Before assign to NetHandler or after release from NetHandler, it's the
  NetEvent's responsibility to deallocate itself.

 */

//
// NetHandler
//
// A NetHandler handles the Network IO operations.  It maintains
// lists of operations at multiples of it's periodicity.
//
class NetHandler : public Continuation, public EThread::LoopTailHandler
{
  using self_type = NetHandler; ///< Self reference type.
public:
  // @a thread and @a trigger_event are redundant - you can get the former from the latter.
  // If we don't get rid of @a trigger_event we should remove @a thread.
  EThread *thread      = nullptr;
  Event *trigger_event = nullptr;

  IOStrategy *io_strategy;

  /// configuration settings for managing the active and keep-alive queues
  struct Config {
    uint32_t max_connections_in                 = 0;
    uint32_t max_requests_in                    = 0;
    uint32_t inactive_threshold_in              = 0;
    uint32_t transaction_no_activity_timeout_in = 0;
    uint32_t keep_alive_no_activity_timeout_in  = 0;
    uint32_t default_inactivity_timeout         = 0;

    /** Return the address of the first value in this struct.

        Doing updates is much easier if we treat this config struct as an array.
        Making it a method means the knowledge of which member is the first one
        is localized to this struct, not scattered about.
     */
    uint32_t &
    operator[](int n)
    {
      return *(&max_connections_in + n);
    }
  };
  /** Static global config, set and updated per process.

      This is updated asynchronously and then events are sent to the NetHandler instances per thread
      to copy to the per thread config at a convenient time. Because these are updated independently
      from the command line, the update events just copy a single value from the global to the
      local. This mechanism relies on members being identical types.
  */
  static Config global_config;
  Config config; ///< Per thread copy of the @c global_config
  // Active and keep alive queue values that depend on other configuration values.
  // These are never updated directly, they are computed from other config values.
  uint32_t max_connections_per_thread_in = 0;
  uint32_t max_requests_per_thread_in    = 0;
  /// Number of configuration items in @c Config.
  static constexpr int CONFIG_ITEM_COUNT = sizeof(Config) / sizeof(uint32_t);
  /// Which members of @c Config the per thread values depend on.
  /// If one of these is updated, the per thread values must also be updated.
  static const std::bitset<CONFIG_ITEM_COUNT> config_value_affects_per_thread_value;
  /// Set of thread types in which nethandlers are active.
  /// This enables signaling the correct instances when the configuration is updated.
  /// Event type threads that use @c NetHandler must set the corresponding bit.
  static std::bitset<std::numeric_limits<unsigned int>::digits> active_thread_types;

  int mainNetEvent(int event, Event *data);
  int waitForActivity(ink_hrtime timeout) override;

  /// Per process initialization logic.
  static void init_for_process();
  /// Update configuration values that are per thread and depend on other configuration values.
  void configure_per_thread_values();

  // Signal the epoll_wait to terminate.
  void signalActivity() override;

  NetHandler();

private:
  /// Static method used as the callback for runtime configuration updates.
  static int update_nethandler_config(const char *name, RecDataT, RecData data, void *);
};

static inline NetHandler *
get_NetHandler(EThread *t)
{
  return static_cast<NetHandler *>(ETHREAD_GET_PTR(t, unix_netProcessor.netHandler_offset));
}

enum ThrottleType {
  ACCEPT,
  CONNECT,
};

TS_INLINE int
net_connections_to_throttle(ThrottleType t)
{
  double headroom = t == ACCEPT ? NET_THROTTLE_ACCEPT_HEADROOM : NET_THROTTLE_CONNECT_HEADROOM;
  int64_t sval    = 0;

  NET_READ_GLOBAL_DYN_SUM(net_connections_currently_open_stat, sval);
  int currently_open = static_cast<int>(sval);
  // deal with race if we got to multiple net threads
  if (currently_open < 0) {
    currently_open = 0;
  }
  return static_cast<int>(currently_open * headroom);
}

TS_INLINE void
check_shedding_warning()
{
  ink_hrtime t = Thread::get_hrtime();
  if (t - last_shedding_warning > NET_THROTTLE_MESSAGE_EVERY) {
    last_shedding_warning = t;
    Warning("number of connections reaching shedding limit");
  }
}

TS_INLINE bool
check_net_throttle(ThrottleType t)
{
  int connections = net_connections_to_throttle(t);

  if (net_connections_throttle != 0 && connections >= net_connections_throttle) {
    return true;
  }

  return false;
}

TS_INLINE void
check_throttle_warning(ThrottleType type)
{
  ink_hrtime t = Thread::get_hrtime();
  if (t - last_throttle_warning > NET_THROTTLE_MESSAGE_EVERY) {
    last_throttle_warning = t;
    int connections       = net_connections_to_throttle(type);
    Warning("too many connections, throttling.  connection_type=%s, current_connections=%d, net_connections_throttle=%d",
            type == ACCEPT ? "ACCEPT" : "CONNECT", connections, net_connections_throttle);
  }
}

TS_INLINE int
change_net_connections_throttle(const char *token, RecDataT data_type, RecData value, void *data)
{
  (void)token;
  (void)data_type;
  (void)value;
  (void)data;
  int throttle = fds_limit - THROTTLE_FD_HEADROOM;
  if (fds_throttle == 0) {
    net_connections_throttle = fds_throttle;
  } else if (fds_throttle < 0) {
    net_connections_throttle = throttle;
  } else {
    net_connections_throttle = fds_throttle;
    if (net_connections_throttle > throttle) {
      net_connections_throttle = throttle;
    }
  }
  return 0;
}

// 2  - ignore
// 1  - transient
// 0  - report as warning
// -1 - fatal
TS_INLINE int
accept_error_seriousness(int res)
{
  switch (res) {
  case -ECONNABORTED:
    return 2;
  case -EAGAIN:
  case -ECONNRESET: // for Linux
  case -EPIPE:      // also for Linux
    return 1;
  case -EMFILE:
  case -ENOMEM:
#if defined(ENOSR) && !defined(freebsd)
  case -ENOSR:
#endif
    ink_assert(!"throttling misconfigured: set too high");
#ifdef ENOBUFS
  // fallthrough
  case -ENOBUFS:
#endif
#ifdef ENFILE
  case -ENFILE:
#endif
    return 0;
  case -EINTR:
    ink_assert(!"should be handled at a lower level");
    return 0;
#if defined(EPROTO) && !defined(freebsd)
  case -EPROTO:
#endif
  case -EOPNOTSUPP:
  case -ENOTSOCK:
  case -ENODEV:
  case -EBADF:
  default:
    return -1;
  }
}

TS_INLINE void
check_transient_accept_error(int res)
{
  ink_hrtime t = Thread::get_hrtime();
  if (!last_transient_accept_error || t - last_transient_accept_error > TRANSIENT_ACCEPT_ERROR_MESSAGE_EVERY) {
    last_transient_accept_error = t;
    Warning("accept thread received transient error: errno = %d", -res);
#if defined(linux)
    if (res == -ENOBUFS || res == -ENFILE) {
      Warning("errno : %d consider a memory upgrade", -res);
    }
#endif
  }
}
