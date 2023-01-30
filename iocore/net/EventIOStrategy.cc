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

#include "I_Continuation.h"
#include "P_EventIO.h"
#include "NetEvent.h"
#include "I_EventIOStrategy.h"
#include "P_Net.h"

#if TS_USE_LINUX_IO_URING
#include "I_IO_URING.h"
//#include "P_UnixNet.h"
#endif

struct PollDescriptor;
struct PollCont : public Continuation {
  EventIOStrategy *io_strategy;
  PollDescriptor *pollDescriptor;
  PollDescriptor *nextPollDescriptor;
  int poll_timeout;

  PollCont(Ptr<ProxyMutex> &m, int pt = 10);
  PollCont(Ptr<ProxyMutex> &m, EventIOStrategy *nh, int pt = 10);
  ~PollCont() override;
  int pollEvent(int, Event *);
  void do_poll(ink_hrtime timeout);
};
class InactivityCop;

class EventIOStrategyImpl
{
public:
  EventIOStrategyImpl(Ptr<ProxyMutex> &, EventIOStrategy *, const EventIOStrategyConfig &);
  void remove_from_keep_alive_queue(NetEvent *ne);
  void remove_from_active_queue(NetEvent *ne);
  void manage_keep_alive_queue();
  bool manage_active_queue(NetEvent *ne, bool ignore_queue_size = false);
  void add_to_keep_alive_queue(NetEvent *ne);
  bool add_to_active_queue(NetEvent *ne);
  void _close_ne(NetEvent *ne, ink_hrtime now, int &handle_event, int &closed, int &total_idle_time, int &total_idle_count);

  void stopCop(NetEvent *ne);
  void startCop(NetEvent *ne);
  void stopIO(NetEvent *ne);
  int startIO(NetEvent *ne);

  inline void
  increment_dyn_stat(int stat, int64_t by = 1)
  {
    if (config.stat_block) {
      RecIncrRawStatSum(config.stat_block, mutex->thread_holding, stat, by);
    }
  }
  inline void
  decrement_dyn_stat(int stat, int64_t by = 1)
  {
    increment_dyn_stat(stat, -by);
  }

  /**
    Release a ne and free it.

  @param ne NetEvent to be detached.
      */
  void free_netevent(NetEvent *ne);

  QueM(NetEvent, NetState, read, ready_link) read_ready_list;
  QueM(NetEvent, NetState, write, ready_link) write_ready_list;
  ASLLM(NetEvent, NetState, read, enable_link) read_enable_list;
  ASLLM(NetEvent, NetState, write, enable_link) write_enable_list;
  Que(NetEvent, open_link) open_list;
  DList(NetEvent, cop_link) cop_list;
  Que(NetEvent, keep_alive_queue_link) keep_alive_queue;
  uint32_t keep_alive_queue_size = 0;
  Que(NetEvent, active_queue_link) active_queue;
  uint32_t active_queue_size = 0;
#ifdef TS_USE_LINUX_IO_URING
  EventIO uring_evio;
#endif
  EThread *thread     = nullptr;
  PollCont *poll_cont = nullptr;
  EventIO *ep         = nullptr;
#if HAVE_EVENTFD
  int evfd = ts::NO_FD;
#else
  int evpipe[2];
#endif

  // this is needed for the stat macros
  Ptr<ProxyMutex> mutex;
  EventIOStrategyConfig config;
  EventIOStrategy *parent;
};

EventIOStrategyImpl::EventIOStrategyImpl(Ptr<ProxyMutex> &m, EventIOStrategy *parent, const EventIOStrategyConfig &config)
  : mutex(m), config(config), parent(parent)
{
  poll_cont = new PollCont(m, config.poll_timeout);
}

TS_INLINE int
EventIOStrategyImpl::startIO(NetEvent *ne)
{
  ink_assert(this->mutex->thread_holding == this_ethread());
  ink_assert(ne->get_thread() == this_ethread());
  int res = 0;

  PollDescriptor *pd = this->poll_cont->pollDescriptor;
  if (ne->ep.start(pd, ne, EVENTIO_READ | EVENTIO_WRITE) < 0) {
    res = errno;
    // EEXIST should be ok, though it should have been cleared before we got back here
    if (errno != EEXIST) {
      Debug("iocore_net", "NetHandler::startIO : failed on EventIO::start, errno = [%d](%s)", errno, strerror(errno));
      return -res;
    }
  }

  if (ne->read.triggered == 1) {
    read_ready_list.enqueue(ne);
  }
  ne->ios = parent;
  return res;
}

TS_INLINE void
EventIOStrategyImpl::stopIO(NetEvent *ne)
{
  ink_release_assert(ne->ios == parent);

  ne->ep.stop();

  read_ready_list.remove(ne);
  write_ready_list.remove(ne);
  if (ne->read.in_enabled_list) {
    read_enable_list.remove(ne);
    ne->read.in_enabled_list = 0;
  }
  if (ne->write.in_enabled_list) {
    write_enable_list.remove(ne);
    ne->write.in_enabled_list = 0;
  }

  ne->nh = nullptr;
}

TS_INLINE void
EventIOStrategyImpl::startCop(NetEvent *ne)
{
  ink_assert(this->mutex->thread_holding == this_ethread());
  ink_release_assert(ne->ios == parent);
  ink_assert(!open_list.in(ne));

  open_list.enqueue(ne);
}

TS_INLINE void
EventIOStrategyImpl::stopCop(NetEvent *ne)
{
  ink_release_assert(ne->ios == parent);

  open_list.remove(ne);
  cop_list.remove(ne);
  remove_from_keep_alive_queue(ne);
  remove_from_active_queue(ne);
}

// INKqa10496
// One Inactivity cop runs on each thread once every second and
// loops through the list of NetEvents and calls the timeouts
class InactivityCop : public Continuation
{
public:
  EventIOStrategyImpl *ios;
  explicit InactivityCop(Ptr<ProxyMutex> &m, EventIOStrategyImpl *ios) : Continuation(m.get()), ios(ios)
  {
    SET_HANDLER(&InactivityCop::check_inactivity);
  }

  int
  check_inactivity(int event, Event *e)
  {
    (void)event;
    ink_hrtime now = Thread::get_hrtime();

    Debug("inactivity_cop_check", "Checking inactivity on Thread-ID #%d", this_ethread()->id);
    // The rest NetEvents in cop_list which are not triggered between InactivityCop runs.
    // Use pop() to catch any closes caused by callbacks.
    while (NetEvent *ne = ios->cop_list.pop()) {
      NetHandler &nh = *ne->nh;
      // If we cannot get the lock don't stop just keep cleaning
      MUTEX_TRY_LOCK(lock, ne->get_mutex(), this_ethread());
      if (!lock.is_locked()) {
        ios->increment_dyn_stat(inactivity_cop_lock_acquire_failure_stat);
        continue;
      }

      if (ne->closed) {
        ios->free_netevent(ne);
        continue;
      }

      // set a default inactivity timeout if one is not set
      // The event `EVENT_INACTIVITY_TIMEOUT` only be triggered if a read
      // or write I/O operation was set by `do_io_read()` or `do_io_write()`.
      if (ne->next_inactivity_timeout_at == 0 && nh.config.default_inactivity_timeout > 0 &&
          (ne->read.enabled || ne->write.enabled)) {
        Debug("inactivity_cop", "vc: %p inactivity timeout not set, setting a default of %d", ne,
              nh.config.default_inactivity_timeout);
        ne->set_default_inactivity_timeout(HRTIME_SECONDS(nh.config.default_inactivity_timeout));
        ios->increment_dyn_stat(default_inactivity_timeout_applied_stat);
      }

      if (ne->next_inactivity_timeout_at && ne->next_inactivity_timeout_at < now) {
        if (ne->is_default_inactivity_timeout()) {
          // track the connections that timed out due to default inactivity
          ios->increment_dyn_stat(default_inactivity_timeout_count_stat);
        }
        if (ios->keep_alive_queue.in(ne)) {
          // only stat if the connection is in keep-alive, there can be other inactivity timeouts
          ink_hrtime diff = (now - (ne->next_inactivity_timeout_at - ne->inactivity_timeout_in)) / HRTIME_SECOND;
          ios->increment_dyn_stat(keep_alive_queue_timeout_total_stat, diff);
          ios->increment_dyn_stat(keep_alive_queue_timeout_count_stat);
        }
        Debug("inactivity_cop_verbose", "ne: %p now: %" PRId64 " timeout at: %" PRId64 " timeout in: %" PRId64, ne,
              ink_hrtime_to_sec(now), ne->next_inactivity_timeout_at, ne->inactivity_timeout_in);
        ne->callback(VC_EVENT_INACTIVITY_TIMEOUT, e);
      } else if (ne->next_activity_timeout_at && ne->next_activity_timeout_at < now) {
        Debug("inactivity_cop_verbose", "active ne: %p now: %" PRId64 " timeout at: %" PRId64 " timeout in: %" PRId64, ne,
              ink_hrtime_to_sec(now), ne->next_activity_timeout_at, ne->active_timeout_in);
        ne->callback(VC_EVENT_ACTIVE_TIMEOUT, e);
      }
    }
    // The cop_list is empty now.
    // Let's reload the cop_list from open_list again.
    forl_LL(NetEvent, ne, ios->open_list)
    {
      if (ne->get_thread() == this_ethread()) {
        ios->cop_list.push(ne);
      }
    }
    // NetHandler will remove NetEvent from cop_list if it is triggered.
    // As the NetHandler runs, the number of NetEvents in the cop_list is decreasing.
    // NetHandler runs 100 times maximum between InactivityCop runs.
    // Therefore we don't have to check all the NetEvents as much as open_list.

    // Cleanup the active and keep-alive queues periodically
    ios->manage_active_queue(nullptr, true); // close any connections over the active timeout
    ios->manage_keep_alive_queue();

    return 0;
  }
};

void
EventIOStrategy::net_signal_hook_callback()
{
#if HAVE_EVENTFD
  uint64_t counter;
  ATS_UNUSED_RETURN(::read(impl->evfd, &counter, sizeof(uint64_t)));
#elif TS_USE_PORT
  /* Nothing to drain or do */
#else
  char dummy[1024];
  ATS_UNUSED_RETURN(read(thread->evpipe[0], &dummy[0], 1024));
#endif
}

int
EventIOStrategy::waitForActivity(ink_hrtime timeout)
{
  EventIO *epd = nullptr;
#if TS_USE_LINUX_IO_URING
  IOUringContext *ur = IOUringContext::local_context();
  bool servicedh     = false;
#endif

  process_enabled_list();

#if TS_USE_LINUX_IO_URING
  ur->submit();
#endif

  // Polling event by PollCont
  PollCont *p = impl->poll_cont;
  p->do_poll(timeout);

  // Get & Process polling result
  PollDescriptor *pd = p->pollDescriptor;
  NetEvent *ne       = nullptr;
  for (int x = 0; x < pd->result; x++) {
    epd = static_cast<EventIO *> get_ev_data(pd, x);
    if (epd->type == EventIO::EVENTIO_READWRITE_VC) {
      ne = static_cast<NetEvent *>(epd->_user);
      // Remove triggered NetEvent from cop_list because it won't be timeout before next InactivityCop runs.
      if (impl->cop_list.in(ne)) {
        impl->cop_list.remove(ne);
      }
      int flags = get_ev_events(pd, x);
      if (flags & (EVENTIO_ERROR)) {
        ne->set_error_from_socket();
      }
      if (flags & (EVENTIO_READ)) {
        ne->read.triggered = 1;
        if (!impl->read_ready_list.in(ne)) {
          impl->read_ready_list.enqueue(ne);
        }
      }
      if (flags & (EVENTIO_WRITE)) {
        ne->write.triggered = 1;
        if (!impl->write_ready_list.in(ne)) {
          impl->write_ready_list.enqueue(ne);
        }
      } else if (!(flags & (EVENTIO_READ))) {
        Debug("iocore_net_main", "Unhandled epoll event: 0x%04x", flags);
        // In practice we sometimes see EPOLLERR and EPOLLHUP through there
        // Anything else would be surprising
        ink_assert((flags & ~(EVENTIO_ERROR)) == 0);
        ne->write.triggered = 1;
        if (!impl->write_ready_list.in(ne)) {
          impl->write_ready_list.enqueue(ne);
        }
      }
    } else if (epd->type == EventIO::EVENTIO_DNS_CONNECTION) {
      /*
      if (epd->_user != nullptr) {
        static_cast<DNSConnection *>(epd->_user)->trigger(); // Make sure the DNSHandler for this con knows we triggered
#if defined(USE_EDGE_TRIGGER)
        epd->refresh(EVENTIO_READ);
#endif
      }
       */
    } else if (epd->type == EventIO::EVENTIO_ASYNC_SIGNAL) {
      net_signal_hook_callback();
    } else if (epd->type == EventIO::EVENTIO_NETACCEPT) {
      impl->thread->schedule_imm(static_cast<NetAccept *>(epd->_user));
#if TS_USE_LINUX_IO_URING
    } else if (epd->type == EventIO::EVENTIO_IO_URING) {
      servicedh = true;
#endif
    }
    ev_next_event(pd, x);
  }

  pd->result = 0;

  process_ready_list();

#if TS_USE_LINUX_IO_URING
  if (servicedh) {
    ur->service();
  }
#endif

  return EVENT_CONT;
}

void
EventIOStrategy::signalActivity()
{
#if HAVE_EVENTFD
  uint64_t counter = 1;
  ATS_UNUSED_RETURN(::write(impl->evfd, &counter, sizeof(uint64_t)));
#elif TS_USE_PORT
  PollDescriptor *pd = get_PollDescriptor(thread);
  ATS_UNUSED_RETURN(port_send(pd->port_fd, 0, thread->ep));
#else
  char dummy = 1;
  ATS_UNUSED_RETURN(write(thread->evpipe[1], &dummy, 1));
#endif
}

//
// Move VC's enabled on a different thread to the ready list
//
void
EventIOStrategy::process_enabled_list()
{
  NetEvent *ne = nullptr;

  SListM(NetEvent, NetState, read, enable_link) rq(impl->read_enable_list.popall());
  while ((ne = rq.pop())) {
    ne->ep.modify(EVENTIO_READ);
    ne->ep.refresh(EVENTIO_READ);
    ne->read.in_enabled_list = 0;
    if ((ne->read.enabled && ne->read.triggered) || ne->closed) {
      impl->read_ready_list.in_or_enqueue(ne);
    }
  }

  SListM(NetEvent, NetState, write, enable_link) wq(impl->write_enable_list.popall());
  while ((ne = wq.pop())) {
    ne->ep.modify(EVENTIO_WRITE);
    ne->ep.refresh(EVENTIO_WRITE);
    ne->write.in_enabled_list = 0;
    if ((ne->write.enabled && ne->write.triggered) || ne->closed) {
      impl->write_ready_list.in_or_enqueue(ne);
    }
  }
}

//
// Walk through the ready list
//
void
EventIOStrategy::process_ready_list()
{
  NetEvent *ne = nullptr;

#if defined(USE_EDGE_TRIGGER)
  // NetEvent *
  while ((ne = impl->read_ready_list.dequeue())) {
    // Initialize the thread-local continuation flags
    set_cont_flags(ne->get_control_flags());
    if (ne->closed) {
      impl->free_netevent(ne);
    } else if (ne->read.enabled && ne->read.triggered) {
      ne->net_read_io(this, this->impl->thread);
    } else if (!ne->read.enabled) {
      impl->read_ready_list.remove(ne);
#if defined(solaris)
      if (ne->read.triggered && ne->write.enabled) {
        ne->ep.modify(-EVENTIO_READ);
        ne->ep.refresh(EVENTIO_WRITE);
        ne->writeReschedule(this);
      }
#endif
    }
  }
  while ((ne = impl->write_ready_list.dequeue())) {
    set_cont_flags(ne->get_control_flags());
    if (ne->closed) {
      impl->free_netevent(ne);
    } else if (ne->write.enabled && ne->write.triggered) {
      ne->net_write_io(this, impl->thread);
    } else if (!ne->write.enabled) {
      impl->write_ready_list.remove(ne);
#if defined(solaris)
      if (ne->write.triggered && ne->read.enabled) {
        ne->ep.modify(-EVENTIO_WRITE);
        ne->ep.refresh(EVENTIO_READ);
        ne->readReschedule(this);
      }
#endif
    }
  }
#else  /* !USE_EDGE_TRIGGER */
  while ((ne = read_ready_list.dequeue())) {
    set_cont_flags(ne->get_control_flags());
    if (ne->closed)
      free_netevent(ne);
    else if (ne->read.enabled && ne->read.triggered)
      ne->net_read_io(this, this->thread);
    else if (!ne->read.enabled)
      ne->ep.modify(-EVENTIO_READ);
  }
  while ((ne = write_ready_list.dequeue())) {
    set_cont_flags(ne->get_control_flags());
    if (ne->closed)
      free_netevent(ne);
    else if (ne->write.enabled && ne->write.triggered)
      write_to_net(this, ne, this->thread);
    else if (!ne->write.enabled)
      ne->ep.modify(-EVENTIO_WRITE);
  }
#endif /* !USE_EDGE_TRIGGER */
}

//
// Function used to release a NetEvent and free it.
//
void
EventIOStrategyImpl::free_netevent(NetEvent *ne)
{
  EThread *t = this->thread;

  ink_assert(t == this_ethread());
  ink_release_assert(ne->get_thread() == t);
  ink_release_assert(ne->ios == parent);

  // Release ne from InactivityCop
  stopCop(ne);
  // Release ne from NetHandler
  stopIO(ne);
  // Clear and deallocate ne
  ne->free(t);
}

EventIOStrategy::EventIOStrategy(Ptr<ProxyMutex> &m, const EventIOStrategyConfig &config)
  : impl(new EventIOStrategyImpl(m, this, config))
{
#if HAVE_EVENTFD
  impl->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (impl->evfd < 0) {
    if (errno == EINVAL) { // flags invalid for kernel <= 2.6.26
      impl->evfd = eventfd(0, 0);
      if (impl->evfd < 0) {
        Fatal("EThread::EThread: %d=eventfd(0,0),errno(%d)", impl->evfd, errno);
      }
    } else {
      Fatal("EThread::EThread: %d=eventfd(0,EFD_NONBLOCK | EFD_CLOEXEC),errno(%d)", impl->evfd, errno);
    }
  }
#elif TS_USE_PORT
  /* Solaris ports requires no crutches to do cross thread signaling.
   * We'll just port_send the event straight over the port.
   */
#else
  ink_release_assert(pipe(evpipe) >= 0);
  fcntl(evpipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(evpipe[0], F_SETFL, O_NONBLOCK);
  fcntl(evpipe[1], F_SETFD, FD_CLOEXEC);
  fcntl(evpipe[1], F_SETFL, O_NONBLOCK);
#endif
}

void
EventIOStrategy::init_for_thread(EThread *thread)
{
  impl->thread = thread;

  // TODO(cmcfarlen): remove this from EThread storage
  impl->poll_cont    = new PollCont(thread->mutex, this, impl->config.poll_timeout);
  PollDescriptor *pd = impl->poll_cont->pollDescriptor;

  impl->ep = static_cast<EventIO *>(ats_malloc(sizeof(EventIO)));
  new (impl->ep) EventIO();
  impl->ep->type = EventIO::EVENTIO_ASYNC_SIGNAL;

#if HAVE_EVENTFD
  impl->ep->start(pd, impl->evfd, nullptr, EVENTIO_READ);
#else
  thread->ep->start(pd, thread->evpipe[0], nullptr, EVENTIO_READ);
#endif

  InactivityCop *inactivityCop = new InactivityCop(impl->mutex, impl);
  thread->schedule_every(inactivityCop, HRTIME_SECONDS(impl->config.inactivity_check_frequency));

#if TS_USE_LINUX_IO_URING
  impl->uring_evio.type = EventIO::EVENTIO_IO_URING;
  impl->uring_evio.start(pd, IOUringContext::local_context()->register_eventfd(), nullptr, EVENTIO_READ);
#endif
}

PollCont::PollCont(Ptr<ProxyMutex> &m, int pt)
  : Continuation(m.get()), io_strategy(nullptr), nextPollDescriptor(nullptr), poll_timeout(pt)
{
  pollDescriptor = new PollDescriptor();
  SET_HANDLER(&PollCont::pollEvent);
}

PollCont::PollCont(Ptr<ProxyMutex> &m, EventIOStrategy *es, int pt)
  : Continuation(m.get()), io_strategy(es), nextPollDescriptor(nullptr), poll_timeout(pt)
{
  pollDescriptor = new PollDescriptor();
  SET_HANDLER(&PollCont::pollEvent);
}

PollCont::~PollCont()
{
  delete pollDescriptor;
  if (nextPollDescriptor != nullptr) {
    delete nextPollDescriptor;
  }
}

//
// PollCont continuation which does the epoll_wait
// and stores the resultant events in ePoll_Triggered_Events
//
int
PollCont::pollEvent(int, Event *)
{
  this->do_poll(-1);
  return EVENT_CONT;
}

void
PollCont::do_poll(ink_hrtime timeout)
{
  if (likely(io_strategy)) {
    /* checking to see whether there are connections on the ready_queue (either read or write) that need processing [ebalsa] */
    if (likely(!io_strategy->impl->read_ready_list.empty() || !io_strategy->impl->write_ready_list.empty() ||
               !io_strategy->impl->read_enable_list.empty() || !io_strategy->impl->write_enable_list.empty())) {
      Debug("iocore_net_poll", "rrq: %d, wrq: %d, rel: %d, wel: %d", io_strategy->impl->read_ready_list.empty(),
            io_strategy->impl->write_ready_list.empty(), io_strategy->impl->read_enable_list.empty(),
            io_strategy->impl->write_enable_list.empty());
      poll_timeout = 0; // poll immediately returns -- we have triggered stuff to process right now
    } else if (timeout >= 0) {
      poll_timeout = ink_hrtime_to_msec(timeout);
    } else {
      poll_timeout = 10; // TODO(cmcfarlen): pass in config - net_config_poll_timeout;
    }
  }
// wait for fd's to trigger, or don't wait if timeout is 0
#if TS_USE_EPOLL
  pollDescriptor->result =
    epoll_wait(pollDescriptor->epoll_fd, pollDescriptor->ePoll_Triggered_Events, POLL_DESCRIPTOR_SIZE, poll_timeout);
  Debug("v_iocore_net_poll", "[PollCont::pollEvent] epoll_fd: %d, timeout: %d, results: %d", pollDescriptor->epoll_fd, poll_timeout,
        pollDescriptor->result);
#elif TS_USE_KQUEUE
  struct timespec tv;
  tv.tv_sec  = poll_timeout / 1000;
  tv.tv_nsec = 1000000 * (poll_timeout % 1000);
  pollDescriptor->result =
    kevent(pollDescriptor->kqueue_fd, nullptr, 0, pollDescriptor->kq_Triggered_Events, POLL_DESCRIPTOR_SIZE, &tv);
  NetDebug("v_iocore_net_poll", "[PollCont::pollEvent] kqueue_fd: %d, timeout: %d, results: %d", pollDescriptor->kqueue_fd,
           poll_timeout, pollDescriptor->result);
#elif TS_USE_PORT
  int retval;
  timespec_t ptimeout;
  ptimeout.tv_sec  = poll_timeout / 1000;
  ptimeout.tv_nsec = 1000000 * (poll_timeout % 1000);
  unsigned nget    = 1;
  if ((retval = port_getn(pollDescriptor->port_fd, pollDescriptor->Port_Triggered_Events, POLL_DESCRIPTOR_SIZE, &nget, &ptimeout)) <
      0) {
    pollDescriptor->result = 0;
    switch (errno) {
    case EINTR:
    case EAGAIN:
    case ETIME:
      if (nget > 0) {
        pollDescriptor->result = (int)nget;
      }
      break;
    default:
      ink_assert(!"unhandled port_getn() case:");
      break;
    }
  } else {
    pollDescriptor->result = (int)nget;
  }
  NetDebug("v_iocore_net_poll", "[PollCont::pollEvent] %d[%s]=port_getn(%d,%p,%d,%d,%d),results(%d)", retval,
           retval < 0 ? strerror(errno) : "ok", pollDescriptor->port_fd, pollDescriptor->Port_Triggered_Events,
           POLL_DESCRIPTOR_SIZE, nget, poll_timeout, pollDescriptor->result);
#else
#error port me
#endif
}

void
EventIOStrategyImpl::add_to_keep_alive_queue(NetEvent *ne)
{
  Debug("net_queue", "NetEvent: %p", ne);
  ink_assert(mutex->thread_holding == this_ethread());

  if (keep_alive_queue.in(ne)) {
    // already in the keep-alive queue, move the head
    keep_alive_queue.remove(ne);
  } else {
    // in the active queue or no queue, new to this queue
    remove_from_active_queue(ne);
    ++keep_alive_queue_size;
  }
  keep_alive_queue.enqueue(ne);

  // if keep-alive queue is over size then close connections
  manage_keep_alive_queue();
}

void
EventIOStrategyImpl::remove_from_keep_alive_queue(NetEvent *ne)
{
  Debug("net_queue", "NetEvent: %p", ne);
  ink_assert(mutex->thread_holding == this_ethread());

  if (keep_alive_queue.in(ne)) {
    keep_alive_queue.remove(ne);
    --keep_alive_queue_size;
  }
}
bool
EventIOStrategyImpl::add_to_active_queue(NetEvent *ne)
{
  NetHandler *nh = ne->nh;
  Debug("net_queue", "NetEvent: %p", ne);
  Debug("net_queue", "max_connections_per_thread_in: %d active_queue_size: %d keep_alive_queue_size: %d",
        nh->max_connections_per_thread_in, active_queue_size, keep_alive_queue_size);
  ink_assert(mutex->thread_holding == this_ethread());

  bool active_queue_full = false;

  // if active queue is over size then close inactive connections
  if (manage_active_queue(ne) == false) {
    active_queue_full = true;
  }

  if (active_queue.in(ne)) {
    // already in the active queue, move the head
    active_queue.remove(ne);
  } else {
    if (active_queue_full) {
      // there is no room left in the queue
      increment_dyn_stat(net_requests_max_throttled_in_stat, 1);
      return false;
    }
    // in the keep-alive queue or no queue, new to this queue
    remove_from_keep_alive_queue(ne);
    ++active_queue_size;
  }
  active_queue.enqueue(ne);

  return true;
}

void
EventIOStrategyImpl::remove_from_active_queue(NetEvent *ne)
{
  Debug("net_queue", "NetEvent: %p", ne);
  ink_assert(mutex->thread_holding == this_ethread());

  if (active_queue.in(ne)) {
    active_queue.remove(ne);
    --active_queue_size;
  }
}

bool
EventIOStrategyImpl::manage_active_queue(NetEvent *enabling_ne, bool ignore_queue_size)
{
  NetHandler *nh                 = enabling_ne->nh;
  const int total_connections_in = active_queue_size + keep_alive_queue_size;
  Debug("v_net_queue",
        "max_connections_per_thread_in: %d max_requests_per_thread_in: %d total_connections_in: %d "
        "active_queue_size: %d keep_alive_queue_size: %d",
        nh->max_connections_per_thread_in, nh->max_requests_per_thread_in, total_connections_in, active_queue_size,
        keep_alive_queue_size);

  if (!nh->max_requests_per_thread_in) {
    // active queue has no max
    return true;
  }

  if (ignore_queue_size == false && nh->max_requests_per_thread_in > active_queue_size) {
    return true;
  }

  ink_hrtime now = Thread::get_hrtime();

  // loop over the non-active connections and try to close them
  NetEvent *ne         = active_queue.head;
  NetEvent *ne_next    = nullptr;
  int closed           = 0;
  int handle_event     = 0;
  int total_idle_time  = 0;
  int total_idle_count = 0;
  for (; ne != nullptr; ne = ne_next) {
    ne_next = ne->active_queue_link.next;
    // It seems dangerous closing the current ne at this point
    // Let the activity_cop deal with it
    if (ne == enabling_ne) {
      continue;
    }
    if ((ne->next_inactivity_timeout_at && ne->next_inactivity_timeout_at <= now) ||
        (ne->next_activity_timeout_at && ne->next_activity_timeout_at <= now)) {
      _close_ne(ne, now, handle_event, closed, total_idle_time, total_idle_count);
    }
    if (ignore_queue_size == false && nh->max_requests_per_thread_in > active_queue_size) {
      return true;
    }
  }

  if (nh->max_requests_per_thread_in > active_queue_size) {
    return true;
  }

  return false; // failed to make room in the queue, all connections are active
}

void
EventIOStrategyImpl::manage_keep_alive_queue()
{
  uint32_t total_connections_in = active_queue_size + keep_alive_queue_size;
  ink_hrtime now                = Thread::get_hrtime();

  Debug("v_net_queue", "max_connections_per_thread_in: %d total_connections_in: %d active_queue_size: %d keep_alive_queue_size: %d",
        config.max_connections_per_thread_in, total_connections_in, active_queue_size, keep_alive_queue_size);

  if (!config.max_connections_per_thread_in || total_connections_in <= config.max_connections_per_thread_in) {
    return;
  }

  // loop over the non-active connections and try to close them
  NetEvent *ne_next    = nullptr;
  int closed           = 0;
  int handle_event     = 0;
  int total_idle_time  = 0;
  int total_idle_count = 0;
  for (NetEvent *ne = keep_alive_queue.head; ne != nullptr; ne = ne_next) {
    ne_next = ne->keep_alive_queue_link.next;
    _close_ne(ne, now, handle_event, closed, total_idle_time, total_idle_count);

    total_connections_in = active_queue_size + keep_alive_queue_size;
    if (total_connections_in <= config.max_connections_per_thread_in) {
      break;
    }
  }

  if (total_idle_count > 0) {
    Debug("net_queue", "max cons: %d active: %d idle: %d already closed: %d, close event: %d mean idle: %d",
          config.max_connections_per_thread_in, total_connections_in, keep_alive_queue_size, closed, handle_event,
          total_idle_time / total_idle_count);
  }
}

void
EventIOStrategyImpl::_close_ne(NetEvent *ne, ink_hrtime now, int &handle_event, int &closed, int &total_idle_time,
                               int &total_idle_count)
{
  if (ne->get_thread() != this_ethread()) {
    return;
  }
  MUTEX_TRY_LOCK(lock, ne->get_mutex(), this_ethread());
  if (!lock.is_locked()) {
    return;
  }
  ink_hrtime diff = (now - (ne->next_inactivity_timeout_at - ne->inactivity_timeout_in)) / HRTIME_SECOND;
  if (diff > 0) {
    total_idle_time += diff;
    ++total_idle_count;
    increment_dyn_stat(keep_alive_queue_timeout_total_stat, diff);
    increment_dyn_stat(keep_alive_queue_timeout_count_stat);
  }
  Debug("net_queue", "closing connection NetEvent=%p idle: %u now: %" PRId64 " at: %" PRId64 " in: %" PRId64 " diff: %" PRId64, ne,
        keep_alive_queue_size, ink_hrtime_to_sec(now), ink_hrtime_to_sec(ne->next_inactivity_timeout_at),
        ink_hrtime_to_sec(ne->inactivity_timeout_in), diff);
  if (ne->closed) {
    free_netevent(ne);
    ++closed;
  } else {
    ne->next_inactivity_timeout_at = now;
    // create a dummy event
    Event event;
    event.ethread = this_ethread();
    if (ne->inactivity_timeout_in && ne->next_inactivity_timeout_at <= now) {
      if (ne->callback(VC_EVENT_INACTIVITY_TIMEOUT, &event) == EVENT_DONE) {
        ++handle_event;
      }
    } else if (ne->active_timeout_in && ne->next_activity_timeout_at <= now) {
      if (ne->callback(VC_EVENT_ACTIVE_TIMEOUT, &event) == EVENT_DONE) {
        ++handle_event;
      }
    }
  }
}

NetEvent *
vio_to_ne(VIO *)
{
  // TODO(cmcfarlen): Get a NetEvent from a vio somehow
  return nullptr;
}

/** Disable reading on the NetEvent @a ne.
     @param nh Nethandler that owns @a ne.
     @param ne The @c NetEvent to modify.

- If write is already disable, also disable the inactivity timeout.
                                 - clear read enabled flag.
                                 - Remove the @c epoll READ flag.
                                 - Take @a ne out of the read ready list.
                                       */
void
EventIOStrategy::read_disable(VIO *vio)
{
  NetEvent *ne = vio_to_ne(vio);
  if (!ne->write.enabled) {
    // Clear the next scheduled inactivity time, but don't clear inactivity_timeout_in,
    // so the current timeout is used when the NetEvent is reenabled and not the default inactivity timeout
    ne->next_inactivity_timeout_at = 0;
    Debug("socket", "read_disable updating inactivity_at %" PRId64 ", NetEvent=%p", ne->next_inactivity_timeout_at, ne);
  }
  ne->read.enabled = 0;
  impl->read_ready_list.remove(ne);
  ne->ep.modify(-EVENTIO_READ);
}

/** Disable writing on the NetEvent @a ne.
     @param nh Nethandler that owns @a ne.
     @param ne The @c NetEvent to modify.

     - If read is already disable, also disable the inactivity timeout.
     - clear write enabled flag.
     - Remove the @c epoll WRITE flag.
     - Take @a ne out of the write ready list.
*/
void
EventIOStrategy::write_disable(VIO *vio)
{
  NetEvent *ne = vio_to_ne(vio);
  if (!ne->read.enabled) {
    // Clear the next scheduled inactivity time, but don't clear inactivity_timeout_in,
    // so the current timeout is used when the NetEvent is reenabled and not the default inactivity timeout
    ne->next_inactivity_timeout_at = 0;
    Debug("socket", "write_disable updating inactivity_at %" PRId64 ", NetEvent=%p", ne->next_inactivity_timeout_at, ne);
  }
  ne->write.enabled = 0;
  impl->write_ready_list.remove(ne);
  ne->ep.modify(-EVENTIO_WRITE);
}

VIO *
EventIOStrategy::read(IOCompletionTarget *, int fd, int64_t nbytes, MIOBuffer *buf)
{
  return nullptr;
}
VIO *
EventIOStrategy::write(IOCompletionTarget *, int fd, int64_t nbytes, MIOBuffer *buf)
{
  return nullptr;
}
Action *
EventIOStrategy::accept(IOCompletionTarget *, int fd, int flags)
{
  return nullptr;
}
Action *
EventIOStrategy::connect(IOCompletionTarget *, sockaddr const *target)
{
  return nullptr;
}

// TODO(cmcfarlen): encapsulate all of this nonsense

#ifdef FIX_THIS_SHIT
//
// Reschedule a UnixNetVConnection by moving it
// onto or off of the ready_list
//
static inline void
read_reschedule(NetHandler *nh, UnixNetVConnection *vc)
{
  vc->ep.refresh(EVENTIO_READ);
  if (vc->read.triggered && vc->read.enabled) {
    nh->read_ready_list.in_or_enqueue(vc);
  } else {
    nh->read_ready_list.remove(vc);
  }
}

static inline void
write_reschedule(NetHandler *nh, UnixNetVConnection *vc)
{
  vc->ep.refresh(EVENTIO_WRITE);
  if (vc->write.triggered && vc->write.enabled) {
    nh->write_ready_list.in_or_enqueue(vc);
  } else {
    nh->write_ready_list.remove(vc);
  }
}

void
net_activity(UnixNetVConnection *vc, EThread *thread)
{
  Debug("socket", "net_activity updating inactivity %" PRId64 ", NetVC=%p", vc->inactivity_timeout_in, vc);
  (void)thread;
  if (vc->inactivity_timeout_in) {
    vc->next_inactivity_timeout_at = Thread::get_hrtime() + vc->inactivity_timeout_in;
  } else {
    vc->next_inactivity_timeout_at = 0;
  }
}

//
// Signal an event
//
static inline int
read_signal_and_update(int event, UnixNetVConnection *vc)
{
  vc->recursion++;
  if (vc->read.vio.cont && vc->read.vio.mutex == vc->read.vio.cont->mutex) {
    vc->read.vio.cont->handleEvent(event, &vc->read.vio);
  } else {
    if (vc->read.vio.cont) {
      Note("read_signal_and_update: mutexes are different? vc=%p, event=%d", vc, event);
    }
    switch (event) {
    case VC_EVENT_EOS:
    case VC_EVENT_ERROR:
    case VC_EVENT_ACTIVE_TIMEOUT:
    case VC_EVENT_INACTIVITY_TIMEOUT:
      Debug("inactivity_cop", "event %d: null read.vio cont, closing vc %p", event, vc);
      vc->closed = 1;
      break;
    default:
      Error("Unexpected event %d for vc %p", event, vc);
      ink_release_assert(0);
      break;
    }
  }
  if (!--vc->recursion && vc->closed) {
    /* BZ  31932 */
    ink_assert(vc->thread == this_ethread());
    vc->nh->free_netevent(vc);
    return EVENT_DONE;
  } else {
    return EVENT_CONT;
  }
}

static inline int
write_signal_and_update(int event, UnixNetVConnection *vc)
{
  vc->recursion++;
  if (vc->write.vio.cont && vc->write.vio.mutex == vc->write.vio.cont->mutex) {
    vc->write.vio.cont->handleEvent(event, &vc->write.vio);
  } else {
    if (vc->write.vio.cont) {
      Note("write_signal_and_update: mutexes are different? vc=%p, event=%d", vc, event);
    }
    switch (event) {
    case VC_EVENT_EOS:
    case VC_EVENT_ERROR:
    case VC_EVENT_ACTIVE_TIMEOUT:
    case VC_EVENT_INACTIVITY_TIMEOUT:
      Debug("inactivity_cop", "event %d: null write.vio cont, closing vc %p", event, vc);
      vc->closed = 1;
      break;
    default:
      Error("Unexpected event %d for vc %p", event, vc);
      ink_release_assert(0);
      break;
    }
  }
  if (!--vc->recursion && vc->closed) {
    /* BZ  31932 */
    ink_assert(vc->thread == this_ethread());
    vc->nh->free_netevent(vc);
    return EVENT_DONE;
  } else {
    return EVENT_CONT;
  }
}

static inline int
read_signal_done(int event, NetHandler *nh, UnixNetVConnection *vc)
{
  vc->read.enabled = 0;
  if (read_signal_and_update(event, vc) == EVENT_DONE) {
    return EVENT_DONE;
  } else {
    read_reschedule(nh, vc);
    return EVENT_CONT;
  }
}

static inline int
write_signal_done(int event, NetHandler *nh, UnixNetVConnection *vc)
{
  vc->write.enabled = 0;
  if (write_signal_and_update(event, vc) == EVENT_DONE) {
    return EVENT_DONE;
  } else {
    write_reschedule(nh, vc);
    return EVENT_CONT;
  }
}

static inline int
read_signal_error(NetHandler *nh, UnixNetVConnection *vc, int lerrno)
{
  vc->lerrno = lerrno;
  return read_signal_done(VC_EVENT_ERROR, nh, vc);
}

static inline int
write_signal_error(NetHandler *nh, UnixNetVConnection *vc, int lerrno)
{
  vc->lerrno = lerrno;
  return write_signal_done(VC_EVENT_ERROR, nh, vc);
}

// Read the data for a UnixNetVConnection.
// Rescheduling the UnixNetVConnection by moving the VC
// onto or off of the ready_list.
// Had to wrap this function with net_read_io for SSL.
static void
read_from_net(NetHandler *nh, UnixNetVConnection *vc, EThread *thread)
{
  NetState *s       = &vc->read;
  ProxyMutex *mutex = thread->mutex.get();
  int64_t r         = 0;

  MUTEX_TRY_LOCK(lock, s->vio.mutex, thread);

  if (!lock.is_locked()) {
    read_reschedule(nh, vc);
    return;
  }

  // It is possible that the closed flag got set from HttpSessionManager in the
  // global session pool case.  If so, the closed flag should be stable once we get the
  // s->vio.mutex (the global session pool mutex).
  if (vc->closed) {
    vc->nh->free_netevent(vc);
    return;
  }
  // if it is not enabled.
  if (!s->enabled || s->vio.op != VIO::READ || s->vio.is_disabled()) {
    read_disable(nh, vc);
    return;
  }

  MIOBufferAccessor &buf = s->vio.buffer;
  ink_assert(buf.writer());

  // if there is nothing to do, disable connection
  int64_t ntodo = s->vio.ntodo();
  if (ntodo <= 0) {
    read_disable(nh, vc);
    return;
  }
  int64_t toread = buf.writer()->write_avail();
  if (toread > ntodo) {
    toread = ntodo;
  }

  // read data
  int64_t rattempted = 0, total_read = 0;
  unsigned niov = 0;
  IOVec tiovec[NET_MAX_IOV];
  if (toread) {
    IOBufferBlock *b = buf.writer()->first_write_block();
    do {
      niov       = 0;
      rattempted = 0;
      while (b && niov < NET_MAX_IOV) {
        int64_t a = b->write_avail();
        if (a > 0) {
          tiovec[niov].iov_base = b->_end;
          int64_t togo          = toread - total_read - rattempted;
          if (a > togo) {
            a = togo;
          }
          tiovec[niov].iov_len = a;
          rattempted += a;
          niov++;
          if (a >= togo) {
            break;
          }
        }
        b = b->next.get();
      }

      ink_assert(niov > 0);
      ink_assert(niov <= countof(tiovec));
      struct msghdr msg;

      ink_zero(msg);
      msg.msg_name    = const_cast<sockaddr *>(vc->get_remote_addr());
      msg.msg_namelen = ats_ip_size(vc->get_remote_addr());
      msg.msg_iov     = &tiovec[0];
      msg.msg_iovlen  = niov;
      r               = SocketManager::recvmsg(vc->con.fd, &msg, 0);

      NET_INCREMENT_DYN_STAT(net_calls_to_read_stat);

      total_read += rattempted;
    } while (rattempted && r == rattempted && total_read < toread);

    // if we have already moved some bytes successfully, summarize in r
    if (total_read != rattempted) {
      if (r <= 0) {
        r = total_read - rattempted;
      } else {
        r = total_read - rattempted + r;
      }
    }
    // check for errors
    if (r <= 0) {
      if (r == -EAGAIN || r == -ENOTCONN) {
        NET_INCREMENT_DYN_STAT(net_calls_to_read_nodata_stat);
        vc->read.triggered = 0;
        nh->read_ready_list.remove(vc);
        return;
      }

      if (!r || r == -ECONNRESET) {
        vc->read.triggered = 0;
        nh->read_ready_list.remove(vc);
        read_signal_done(VC_EVENT_EOS, nh, vc);
        return;
      }
      vc->read.triggered = 0;
      read_signal_error(nh, vc, static_cast<int>(-r));
      return;
    }
    NET_SUM_DYN_STAT(net_read_bytes_stat, r);

    // Add data to buffer and signal continuation.
    buf.writer()->fill(r);
#ifdef DEBUG
    if (buf.writer()->write_avail() <= 0) {
      Debug("iocore_net", "read_from_net, read buffer full");
    }
#endif
    s->vio.ndone += r;
    net_activity(vc, thread);
  } else {
    r = 0;
  }

  // Signal read ready, check if user is not done
  if (r) {
    // If there are no more bytes to read, signal read complete
    ink_assert(ntodo >= 0);
    if (s->vio.ntodo() <= 0) {
      read_signal_done(VC_EVENT_READ_COMPLETE, nh, vc);
      Debug("iocore_net", "read_from_net, read finished - signal done");
      return;
    } else {
      if (read_signal_and_update(VC_EVENT_READ_READY, vc) != EVENT_CONT) {
        return;
      }

      // change of lock... don't look at shared variables!
      if (lock.get_mutex() != s->vio.mutex.get()) {
        read_reschedule(nh, vc);
        return;
      }
    }
  }

  // If here are is no more room, or nothing to do, disable the connection
  if (s->vio.ntodo() <= 0 || !s->enabled || !buf.writer()->write_avail()) {
    read_disable(nh, vc);
    return;
  }

  read_reschedule(nh, vc);
}

//
// Write the data for a UnixNetVConnection.
// Rescheduling the UnixNetVConnection when necessary.
//
void
write_to_net(NetHandler *nh, UnixNetVConnection *vc, EThread *thread)
{
  ProxyMutex *mutex = thread->mutex.get();

  NET_INCREMENT_DYN_STAT(net_calls_to_writetonet_stat);
  NET_INCREMENT_DYN_STAT(net_calls_to_writetonet_afterpoll_stat);

  write_to_net_io(nh, vc, thread);
}

void
write_to_net_io(NetHandler *nh, UnixNetVConnection *vc, EThread *thread)
{
  NetState *s       = &vc->write;
  ProxyMutex *mutex = thread->mutex.get();

  MUTEX_TRY_LOCK(lock, s->vio.mutex, thread);

  if (!lock.is_locked() || lock.get_mutex() != s->vio.mutex.get()) {
    write_reschedule(nh, vc);
    return;
  }

  if (vc->has_error()) {
    vc->lerrno = vc->error;
    write_signal_and_update(VC_EVENT_ERROR, vc);
    return;
  }

  // This function will always return true unless
  // vc is an SSLNetVConnection.
  if (!vc->getSSLHandShakeComplete()) {
    if (vc->trackFirstHandshake()) {
      // Eat the first write-ready.  Until the TLS handshake is complete,
      // we should still be under the connect timeout and shouldn't bother
      // the state machine until the TLS handshake is complete
      vc->write.triggered = 0;
      nh->write_ready_list.remove(vc);
    }

    int err, ret;

    if (vc->get_context() == NET_VCONNECTION_OUT) {
      ret = vc->sslStartHandShake(SSL_EVENT_CLIENT, err);
    } else {
      ret = vc->sslStartHandShake(SSL_EVENT_SERVER, err);
    }

    if (ret == EVENT_ERROR) {
      vc->write.triggered = 0;
      write_signal_error(nh, vc, err);
    } else if (ret == SSL_HANDSHAKE_WANT_READ || ret == SSL_HANDSHAKE_WANT_ACCEPT) {
      vc->read.triggered = 0;
      nh->read_ready_list.remove(vc);
      read_reschedule(nh, vc);
    } else if (ret == SSL_HANDSHAKE_WANT_CONNECT || ret == SSL_HANDSHAKE_WANT_WRITE) {
      vc->write.triggered = 0;
      nh->write_ready_list.remove(vc);
      write_reschedule(nh, vc);
    } else if (ret == EVENT_DONE) {
      vc->write.triggered = 1;
      if (vc->write.enabled) {
        nh->write_ready_list.in_or_enqueue(vc);
      }
    } else {
      write_reschedule(nh, vc);
    }

    return;
  }

  // If it is not enabled,add to WaitList.
  if (!s->enabled || s->vio.op != VIO::WRITE) {
    write_disable(nh, vc);
    return;
  }

  // If there is nothing to do, disable
  int64_t ntodo = s->vio.ntodo();
  if (ntodo <= 0) {
    write_disable(nh, vc);
    return;
  }

  MIOBufferAccessor &buf = s->vio.buffer;
  ink_assert(buf.writer());

  // Calculate the amount to write.
  int64_t towrite = buf.reader()->read_avail();
  if (towrite > ntodo) {
    towrite = ntodo;
  }

  int signalled = 0;

  // signal write ready to allow user to fill the buffer
  if (towrite != ntodo && !buf.writer()->high_water()) {
    if (write_signal_and_update(VC_EVENT_WRITE_READY, vc) != EVENT_CONT) {
      return;
    }

    ntodo = s->vio.ntodo();
    if (ntodo <= 0) {
      write_disable(nh, vc);
      return;
    }

    signalled = 1;

    // Recalculate amount to write
    towrite = buf.reader()->read_avail();
    if (towrite > ntodo) {
      towrite = ntodo;
    }
  }

  // if there is nothing to do, disable
  ink_assert(towrite >= 0);
  if (towrite <= 0) {
    write_disable(nh, vc);
    return;
  }

  int needs             = 0;
  int64_t total_written = 0;
  int64_t r             = vc->load_buffer_and_write(towrite, buf, total_written, needs);

  if (total_written > 0) {
    NET_SUM_DYN_STAT(net_write_bytes_stat, total_written);
    s->vio.ndone += total_written;
    net_activity(vc, thread);
  }

  // A write of 0 makes no sense since we tried to write more than 0.
  ink_assert(r != 0);
  // Either we wrote something or got an error.
  // check for errors
  if (r < 0) { // if the socket was not ready, add to WaitList
    if (r == -EAGAIN || r == -ENOTCONN || -r == EINPROGRESS) {
      NET_INCREMENT_DYN_STAT(net_calls_to_write_nodata_stat);
      if ((needs & EVENTIO_WRITE) == EVENTIO_WRITE) {
        vc->write.triggered = 0;
        nh->write_ready_list.remove(vc);
        write_reschedule(nh, vc);
      }

      if ((needs & EVENTIO_READ) == EVENTIO_READ) {
        vc->read.triggered = 0;
        nh->read_ready_list.remove(vc);
        read_reschedule(nh, vc);
      }

      return;
    }

    vc->write.triggered = 0;
    write_signal_error(nh, vc, static_cast<int>(-r));
    return;
  } else {                                        // Wrote data.  Finished without error
    int wbe_event = vc->write_buffer_empty_event; // save so we can clear if needed.

    // If the empty write buffer trap is set, clear it.
    if (!(buf.reader()->is_read_avail_more_than(0))) {
      vc->write_buffer_empty_event = 0;
    }

    // If there are no more bytes to write, signal write complete,
    ink_assert(ntodo >= 0);
    if (s->vio.ntodo() <= 0) {
      write_signal_done(VC_EVENT_WRITE_COMPLETE, nh, vc);
      return;
    }

    int e = 0;
    if (!signalled || (s->vio.ntodo() > 0 && !buf.writer()->high_water())) {
      e = VC_EVENT_WRITE_READY;
    } else if (wbe_event != vc->write_buffer_empty_event) {
      // @a signalled means we won't send an event, and the event values differing means we
      // had a write buffer trap and cleared it, so we need to send it now.
      e = wbe_event;
    }

    if (e) {
      if (write_signal_and_update(e, vc) != EVENT_CONT) {
        return;
      }

      // change of lock... don't look at shared variables!
      if (lock.get_mutex() != s->vio.mutex.get()) {
        write_reschedule(nh, vc);
        return;
      }
    }

    if ((needs & EVENTIO_READ) == EVENTIO_READ) {
      read_reschedule(nh, vc);
    }

    if (!(buf.reader()->is_read_avail_more_than(0))) {
      write_disable(nh, vc);
      return;
    }

    if ((needs & EVENTIO_WRITE) == EVENTIO_WRITE) {
      write_reschedule(nh, vc);
    }

    return;
  }
}
#endif
