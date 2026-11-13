/** @file

  A brief file description

  @par License

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

#include <typeinfo>
#include <bit>
#include <chrono>

#include <tscore/TSSystemState.h>

//////////////////////////////////////////////////////////////////////
//
// The EThread Class
//
/////////////////////////////////////////////////////////////////////
#include "iocore/eventsystem/EThread.h"
#include "iocore/eventsystem/EventProcessor.h"
#include "iocore/eventsystem/Lock.h"
#include "tscore/ink_hrtime.h"
#include "tscore/ink_atomic.h"

#if HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

struct AIOCallback;

#define NO_HEARTBEAT                  -1
#define THREAD_MAX_HEARTBEAT_MSECONDS 60

// !! THIS MUST BE IN THE ENUM ORDER !!
char const *const EThread::Metrics::Slice::STAT_NAME[] = {
  "proxy.process.eventloop.count",       "proxy.process.eventloop.events",
  "proxy.process.eventloop.events.min",  "proxy.process.eventloop.events.max",
  "proxy.process.eventloop.wait",        "proxy.process.eventloop.time.min",
  "proxy.process.eventloop.time.max",    "proxy.process.eventloop.drain.queue.max",
  "proxy.process.eventloop.io.wait.max", "proxy.process.eventloop.io.work.max",
};

int              thread_max_heartbeat_mseconds = THREAD_MAX_HEARTBEAT_MSECONDS;
EThreadLoopHooks ethread_loop_hooks;
int              loop_time_update_probability = 10;
const ink_hrtime DELAY_FOR_RETRY              = HRTIME_MSECONDS(10);
// Longest sleep while Events wait on a mutex. A thread with a lock_wait_slot is woken by the releaser, so its limit only
// bounds a missed wake; a thread without one polls.
const ink_hrtime LOCK_WAIT_POLL     = HRTIME_MSECONDS(1);
const ink_hrtime LOCK_WAIT_FALLBACK = HRTIME_MSECONDS(10);
// Waiters redispatched per mutex per loop pass, so one hot mutex can't monopolize the thread.
constexpr int LOCK_WAIT_BATCH = 32;
// A busy mutex is usually held for about a microsecond, far less than a loop pass, so a waiter spins briefly for it
// before deferring to the next pass; the total per mutex per pass is capped so a long hold still falls back.
const ink_hrtime LOCK_WAIT_SPIN       = HRTIME_USECONDS(2);
const ink_hrtime LOCK_WAIT_SPIN_TOTAL = HRTIME_USECONDS(20);
// Spinners per mutex. When every thread waits on one mutex, their reads of its cache line slow the holder; the rest
// defer to the next pass instead.
constexpr int LOCK_WAIT_MAX_SPINNERS = 1;

namespace
{
std::atomic<int>       lock_wait_next_slot{0};
std::atomic<EThread *> lock_wait_slot_threads[64];

struct LockWaitMetrics {
  ts::Metrics::Counter::AtomicType *parks;
  ts::Metrics::Counter::AtomicType *wakes;
  ts::Metrics::Counter::AtomicType *unwoken;
};

LockWaitMetrics &
lock_wait_metrics()
{
  static LockWaitMetrics m{ts::Metrics::Counter::createPtr("proxy.process.eventloop.lock_wait.parks"),
                           ts::Metrics::Counter::createPtr("proxy.process.eventloop.lock_wait.wakes"),
                           ts::Metrics::Counter::createPtr("proxy.process.eventloop.lock_wait.unwoken_sleeps")};
  return m;
}

void
cpu_pause()
{
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

// Spin until @a m looks free or @a budget runs out. Only reads, so a spinner never takes the line exclusive.
bool
spin_until_free(ProxyMutex *m, ink_hrtime budget)
{
  ink_hrtime const deadline = ink_get_hrtime() + budget;
  for (int i = 1;; ++i) {
    if (__atomic_load_n(&m->thread_holding, __ATOMIC_RELAXED) == nullptr) {
      return true;
    }
    cpu_pause();
    if ((i & 15) == 0 && ink_get_hrtime() >= deadline) {
      return false;
    }
  }
}
} // namespace

// To define a class inherits from Thread:
//   1) Define an independent thread_local static member
//   2) Override Thread::set_specific() and assign that member and call Thread::set_specific()
//   3) Define this_Xthread() which get thread specific data
//   4) Clear thread specific data at destructor function.
//
// The below comments are copied from I_Thread.h
//
// Additionally, the EThread class (derived from Thread) maintains its
// own independent data. All (and only) the threads created in the Event
// Subsystem have this data.
thread_local EThread *EThread::this_ethread_ptr;
int                   EThread::default_wait_interval_ms = 10;

void
EThread::set_specific()
{
  this_ethread_ptr = this;
  Thread::set_specific();
}

void
EThread::cons_common()
{
  MUTEX_TAKE_LOCK(mutex, this);
  mutex->nthread_holding += THREAD_MUTEX_THREAD_HOLDING;

  memset(thread_private, 0, PER_THREAD_DATA);
}

EThread::EThread()
{
  cons_common();
}

EThread::EThread(ThreadType att, int anid) : id(anid), tt(att)
{
  cons_common();
#if HAVE_EVENTFD
  evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (evfd < 0) {
    if (errno == EINVAL) { // flags invalid for kernel <= 2.6.26
      evfd = eventfd(0, 0);
      if (evfd < 0) {
        Fatal("EThread::EThread: %d=eventfd(0,0),errno(%d)", evfd, errno);
      }
    } else {
      Fatal("EThread::EThread: %d=eventfd(0,EFD_NONBLOCK | EFD_CLOEXEC),errno(%d)", evfd, errno);
    }
  }
#else
  ink_release_assert(pipe(evpipe) >= 0);
  fcntl(evpipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(evpipe[0], F_SETFL, O_NONBLOCK);
  fcntl(evpipe[1], F_SETFD, FD_CLOEXEC);
  fcntl(evpipe[1], F_SETFL, O_NONBLOCK);
#endif
}

EThread::EThread(ThreadType att, Event *e) : tt(att), start_event(e)
{
  ink_assert(att == DEDICATED);
  cons_common();
}

// Provide a destructor so that SDK functions which create and destroy
// threads won't have to deal with EThread memory deallocation.
EThread::~EThread()
{
  ink_release_assert(mutex->thread_holding == this);
  if (this_ethread_ptr == this) {
    this_ethread_ptr = nullptr;
  }

  mutex->nthread_holding -= THREAD_MUTEX_THREAD_HOLDING;
  MUTEX_UNTAKE_LOCK(mutex, this);
}

bool
EThread::is_event_type(EventType et)
{
  return (event_types & (1 << static_cast<int>(et))) != 0;
}

void
EThread::set_event_type(EventType et)
{
  event_types |= (1 << static_cast<int>(et));
}

ink_hrtime
EThread::process_event(Event *e, int calling_code, ink_hrtime event_time)
{
  ink_assert((!e->in_the_prot_queue && !e->in_the_priority_queue && !e->in_lock_wait));
  lock_wait_parked = false;
  lock_wait_busy   = nullptr;
  WEAK_MUTEX_TRY_LOCK(lock, e->mutex, this);
  if (!lock.is_locked()) {
    if (e->required_mutex != nullptr) {
      park_lock_wait(e, lock_wait_redispatch);
      lock_wait_parked = true;
      return event_time;
    }
    e->timeout_at = event_time + DELAY_FOR_RETRY;
    EventQueueExternal.enqueue_local(e);
  } else {
    if (e->cancelled) {
      MUTEX_RELEASE(lock);
      free_event(e);
      return event_time;
    }
    // Taken after e->mutex, the order handlers take the two. The handler's own lock of it adopts this hold.
    ProxyMutex *required = e->required_mutex;
    if (required != nullptr) {
      if (!MUTEX_TAKE_TRY_LOCK(required, this)) {
        MUTEX_RELEASE(lock);
        park_lock_wait(e, lock_wait_redispatch);
        lock_wait_parked = true;
        lock_wait_busy   = required;
        return event_time;
      }
      required->adopt_pending = true;
#ifdef ENABLE_LOCK_LEDGER
      if (e->lock_wait_since != 0) {
        lock_ledger::on_parked(required, MakeSourceLocation(), e->lock_wait_since);
        e->lock_wait_since = 0;
      }
#endif
    }
    Continuation *c_temp = e->continuation;

    // Restore the client IP debugging flags
    set_cont_flags(e->continuation->control_flags);

    {
      LOCK_LEDGER_CONTEXT(e->continuation);
      e->continuation->handleEvent(calling_code, e);
    }
    if (loop_time_update_probability == 100) {
      event_time = ink_get_hrtime();
    } else if (loop_time_update_probability > 0) {
      if (static_cast<int>(generator.random() % 100) < loop_time_update_probability) {
        event_time = ink_get_hrtime();
      }
    }
    // A handler that never locked the required mutex leaves the loop's hold behind.
    if (required != nullptr) {
      Mutex_release_unadopted(required, this);
    }
    ink_assert(!e->in_the_priority_queue);
    ink_assert(c_temp == e->continuation);
    MUTEX_RELEASE(lock);
    if (e->period) {
      if (!e->in_the_prot_queue && !e->in_the_priority_queue) {
        if (e->period < 0) {
          e->timeout_at = e->period;
        } else {
          e->timeout_at = event_time + e->period;
        }
        EventQueueExternal.enqueue_local(e);
      }
    } else if (!e->in_the_prot_queue && !e->in_the_priority_queue) {
      free_event(e);
    }
  }
  return event_time;
}

uint64_t
EThread::lock_wait_bit()
{
  if (lock_wait_slot == -2) {
    int slot = lock_wait_next_slot.fetch_add(1, std::memory_order_relaxed);
    if (slot < 64) {
      lock_wait_slot_threads[slot].store(this, std::memory_order_release);
      lock_wait_slot = slot;
    } else {
      lock_wait_slot = -1;
    }
  }
  return lock_wait_slot >= 0 ? uint64_t{1} << lock_wait_slot : 0;
}

bool
EThread::lock_wait_any_free()
{
  for (auto &w : lock_waits) {
    if (!w.events.empty() && __atomic_load_n(&w.mutex->thread_holding, __ATOMIC_RELAXED) == nullptr) {
      return true;
    }
  }
  return false;
}

void
EThread::flush_lock_wait_stats(ink_hrtime now)
{
  if (now - lock_wait_flushed < HRTIME_SECOND) {
    return;
  }
  lock_wait_flushed = now;
  if (lock_wait_parks == 0 && lock_wait_wakes == 0 && lock_wait_unwoken == 0) {
    return;
  }
  auto &m = lock_wait_metrics();
  ts::Metrics::Counter::increment(m.parks, lock_wait_parks);
  ts::Metrics::Counter::increment(m.wakes, lock_wait_wakes);
  ts::Metrics::Counter::increment(m.unwoken, lock_wait_unwoken);
  lock_wait_parks = lock_wait_wakes = lock_wait_unwoken = 0;
}

void
lock_wait_wake(ProxyMutex *m, EThread *t)
{
  // Pairs with the fence after lock_wait_sleeping is set in execute_regular: either the sleeper sees the mutex free,
  // or this sees the sleeper.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  uint64_t mask = m->lock_wait_threads.load(std::memory_order_relaxed);
  if (t != nullptr && t->lock_wait_slot >= 0) {
    mask &= ~(uint64_t{1} << t->lock_wait_slot); // our own waiters run at the end of this pass
  }
  if (mask == 0) {
    return;
  }
  unsigned const start = m->lock_wait_cursor.load(std::memory_order_relaxed) & 63;
  unsigned const slot  = (start + std::countr_zero(std::rotr(mask, static_cast<int>(start)))) & 63;
  m->lock_wait_cursor.store(static_cast<uint8_t>(slot + 1), std::memory_order_relaxed);
  EThread *target = lock_wait_slot_threads[slot].load(std::memory_order_acquire);
  // An awake waiter retries on its next pass, so only a sleeper needs the syscall.
  if (target != nullptr && target->lock_wait_sleeping.load(std::memory_order_relaxed) &&
      target->lock_wait_sleeping.exchange(false, std::memory_order_acq_rel)) {
    target->tail_cb->signalActivity();
    if (t != nullptr && t == this_ethread()) {
      ++t->lock_wait_wakes;
    }
  }
}

void
EThread::park_lock_wait(Event *e, bool at_front)
{
  LockWaitList *list = nullptr;
  for (auto &w : lock_waits) {
    if (w.mutex == e->required_mutex) {
      list = &w;
      break;
    }
    if (list == nullptr && w.events.empty()) {
      list = &w;
    }
  }
  if (list == nullptr) {
    list = &lock_waits.emplace_back();
  }
  list->mutex     = e->required_mutex;
  e->in_lock_wait = true;
  if (list->events.empty() && !list->flagged) {
    if (uint64_t bit = lock_wait_bit(); bit != 0) {
      list->mutex->lock_wait_threads.fetch_or(bit, std::memory_order_seq_cst);
      list->flagged = true;
    }
  }
  ++lock_wait_parks;
#ifdef ENABLE_LOCK_LEDGER
  if (e->lock_wait_since == 0 && lock_ledger::enabled_for(e->required_mutex)) {
    e->lock_wait_since = ink_get_hrtime();
  }
#endif
  if (at_front) {
    list->events.push(e);
  } else {
    list->events.enqueue(e);
  }
  ++lock_wait_count;
}

ink_hrtime
EThread::process_lock_waits(ink_hrtime event_time)
{
  if (lock_wait_count == 0) {
    return event_time;
  }
  lock_wait_redispatch = true;
  // By index: a handler can park on a new mutex and grow the vector.
  for (size_t i = 0; i < lock_waits.size(); ++i) {
    ink_hrtime spun = 0;
    for (int n = 0; n < LOCK_WAIT_BATCH; ++n) {
      Event *e = lock_waits[i].events.dequeue();
      if (e == nullptr) {
        break;
      }
      e->in_lock_wait = false;
      --lock_wait_count;
      event_time = process_event(e, e->callback_event, event_time);
      if (lock_wait_parked) {
        // Back at the front. Retry it if the mutex frees up within the spin budget, else leave it for the next pass.
        if (lock_wait_busy == nullptr || spun >= LOCK_WAIT_SPIN_TOTAL) {
          break;
        }
        ProxyMutex *busy = lock_wait_busy;
        if (busy->lock_wait_spinners.fetch_add(1, std::memory_order_relaxed) >= LOCK_WAIT_MAX_SPINNERS) {
          busy->lock_wait_spinners.fetch_sub(1, std::memory_order_relaxed);
          break;
        }
        ink_hrtime const start  = ink_get_hrtime();
        bool const       freed  = spin_until_free(busy, std::min(LOCK_WAIT_SPIN, LOCK_WAIT_SPIN_TOTAL - spun));
        spun                   += ink_get_hrtime() - start;
        busy->lock_wait_spinners.fetch_sub(1, std::memory_order_relaxed);
        if (!freed) {
          break;
        }
        --n; // the retry doesn't count against the batch
      }
    }
    auto &w = lock_waits[i];
    if (w.flagged && w.events.empty()) {
      w.mutex->lock_wait_threads.fetch_and(~lock_wait_bit(), std::memory_order_relaxed);
      w.flagged = false;
    }
  }
  lock_wait_redispatch = false;
  return event_time;
}

Event *
EThread::schedule_lock_wait_local(Continuation *c, ProxyMutex *m, int callback_event)
{
  if (tt != REGULAR) {
    return nullptr;
  }
  ink_release_assert(this == this_ethread());
  Event *e = EVENT_ALLOC(eventAllocator, this);
  e->init(c, 0, 0);
  e->callback_event     = callback_event;
  e->required_mutex     = m;
  e->ethread            = this;
  e->mutex              = c->mutex;
  e->globally_allocated = false;
  c->control_flags.set_flags(get_cont_flags().get_flags());
  park_lock_wait(e, false);
  return e;
}

ink_hrtime
EThread::process_queue(Que(Event, link) * NegativeQueue, int *ev_count, ink_hrtime event_time)
{
  Event *e;

  // Move events from the external thread safe queues to the local queue.
  EventQueueExternal.dequeue_external();

  // execute all the available external events that have
  // already been dequeued
  while ((e = EventQueueExternal.dequeue_local())) {
    ++(*ev_count);
    if (e->cancelled) {
      free_event(e);
    } else if (!e->timeout_at) { // IMMEDIATE
      ink_assert(e->period == 0);
      event_time = process_event(e, e->callback_event, event_time);
    } else if (e->timeout_at > 0) { // INTERVAL
      EventQueue.enqueue(e, event_time);
    } else { // NEGATIVE
      Event *p = nullptr;
      Event *a = NegativeQueue->head;
      while (a && a->timeout_at > e->timeout_at) {
        p = a;
        a = a->link.next;
      }
      if (!a) {
        NegativeQueue->enqueue(e);
      } else {
        NegativeQueue->insert(e, p);
      }
    }
  }
  return event_time;
}

void
EThread::execute_regular()
{
  ink_hrtime delta;            // time spent in the event loop
  ink_hrtime loop_start_time;  // Time the loop started.
  ink_hrtime loop_finish_time; // Time at the end of the loop.

  loop_start_time = ink_get_hrtime();

  // Track this so we can update on boundary crossing.
  auto prev_slice = this->metrics.prev_slice(metrics._slice.data() + (loop_start_time / HRTIME_SECOND) % Metrics::N_SLICES);

  // A statically initialized instance we can use as a prototype for initializing other instances.
  static const Metrics::Slice SLICE_INIT;

  Metrics::Slice *current_slice{nullptr};

  if (ethread_loop_hooks.start != nullptr) {
    ethread_loop_hooks.start();
  }

  // give priority to immediate events
  while (!TSSystemState::is_event_system_shut_down()) {
    current_slice = metrics._slice.data() + (loop_start_time / HRTIME_SECOND) % Metrics::N_SLICES;
    metrics.current_slice.store(current_slice, std::memory_order_release);
    if (current_slice != prev_slice) {
      // I have observed multi-second event loops in production, making this necessary. [amc]
      do {
        memcpy(prev_slice = this->metrics.next_slice(prev_slice), &SLICE_INIT, sizeof(SLICE_INIT));
      } while (current_slice != prev_slice);
      current_slice->record_loop_start(loop_start_time);
    }
    ++(current_slice->_count); // loop started, bump count.

    EventStatistics statistics = fire_ready_events(loop_start_time);
    if (ethread_loop_hooks.between_events != nullptr) {
      ethread_loop_hooks.between_events();
    }
    ink_hrtime sleep_time = get_sleep_time(statistics.event_time, current_slice);

    // drained the queue by this point
    ink_hrtime post_drain  = statistics.event_time;
    ink_hrtime drain_queue = post_drain - loop_start_time;

    // The watchdog heartbeat is kicked inside the tail handler, bracketing only the blocking poll -
    // see Watchdog::SleepScope. Most of waitForActivity is dispatch work, not sleep, so timestamping
    // around this call would attribute the busiest part of the loop to being asleep.
    // Publish that this thread may sleep with parked Events, then re-check: a release between our last try and here
    // either sees the flag and wakes us, or we see the mutex free and skip the sleep.
    bool lock_wait_sleep = false;
    if (lock_wait_count > 0 && sleep_time > 0 && lock_wait_slot >= 0) {
      lock_wait_sleeping.store(true, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      if (lock_wait_any_free()) {
        lock_wait_sleeping.store(false, std::memory_order_relaxed);
        sleep_time = 0;
      } else {
        lock_wait_sleep = true;
      }
    }

    tail_cb->waitForActivity(sleep_time);

    if (lock_wait_sleep && lock_wait_sleeping.exchange(false, std::memory_order_relaxed)) {
      ++lock_wait_unwoken; // no release woke us: a timeout, or other work arrived
    }

    // loop cleanup
    loop_finish_time = ink_get_hrtime();
    // @a delta can be negative due to time of day adjustments (which apparently happen quite frequently). I
    // tried using the monotonic clock to get around this but it was *very* stuttery (up to hundreds
    // of milliseconds), far too much to be actually used.
    delta           = std::max<ink_hrtime>(0, loop_finish_time - loop_start_time);
    loop_start_time = loop_finish_time;

    metrics.decay();
    metrics.record_loop_time(delta);
    flush_lock_wait_stats(loop_finish_time);
    current_slice->record_event_count(statistics.ev_count);
    current_slice->record_drain_queue(drain_queue);
  }

  if (ethread_loop_hooks.stop != nullptr) {
    ethread_loop_hooks.stop();
  }
}

EThread::EventStatistics
EThread::fire_ready_events(ink_hrtime loop_start_time)
{
  Que(Event, link) NegativeQueue;
  EventStatistics result{0, 0};

  result.event_time = process_queue(&NegativeQueue, &result.ev_count, loop_start_time);

  bool done_one;
  do {
    done_one = false;
    // execute all the eligible internal events
    EventQueue.check_ready(result.event_time, this);
    while (Event *e = EventQueue.dequeue_ready(result.event_time)) {
      ink_assert(e);
      ink_assert(e->timeout_at > 0);
      if (e->cancelled) {
        free_event(e);
      } else {
        done_one          = true;
        result.event_time = process_event(e, e->callback_event, result.event_time);
      }
    }
  } while (done_one);

  // execute any negative (poll) events
  if (NegativeQueue.head) {
    result.event_time = process_queue(&NegativeQueue, &result.ev_count, result.event_time);

    // execute poll events
    while (Event *e = NegativeQueue.dequeue()) {
      result.event_time = process_event(e, EVENT_POLL, result.event_time);
    }
  }

  result.event_time = process_lock_waits(result.event_time);

  return result;
}

ink_hrtime
EThread::get_sleep_time(ink_hrtime event_time, Metrics::Slice *current_slice)
{
  ink_hrtime next_time  = EventQueue.earliest_timeout();
  ink_hrtime sleep_time = next_time - event_time;
  if (sleep_time > 0) {
    if (EventQueueExternal.localQueue.empty()) {
      sleep_time = std::min(sleep_time, HRTIME_MSECONDS(thread_max_heartbeat_mseconds));
    } else {
      // Because of a missed lock, Timed-Event and Negative-Event have been pushed into localQueue for retry in awhile.
      // Therefore, we have to set the limitation of sleep time in order to handle the next retry in time.
      sleep_time = std::min(sleep_time, DELAY_FOR_RETRY);
    }
    if (lock_wait_count > 0) {
      sleep_time = std::min(sleep_time, lock_wait_slot >= 0 ? LOCK_WAIT_FALLBACK : LOCK_WAIT_POLL);
    }
    ++(current_slice->_wait);
  } else {
    sleep_time = 0;
  }
  return sleep_time;
}

//
// void  EThread::execute()
//
// Execute loops forever on:
// Find the earliest event.
// Sleep until the event time or until an earlier event is inserted
// When it's time for the event, try to get the appropriate continuation
// lock. If successful, call the continuation, otherwise put the event back
// into the queue.
//

void
EThread::execute()
{
  // Do the start event first.
  // coverity[lock]
  if (start_event) {
    {
      SCOPED_MUTEX_LOCK(lock, start_event->mutex, this);
      start_event->continuation->handleEvent(EVENT_IMMEDIATE, start_event);
    }
    free_event(start_event);
    start_event = nullptr;
  }

  switch (tt) {
  case REGULAR: {
    /* The Event Thread has two status: busy and sleep:
     *   - Keep `EThread::lock` locked while Event Thread is busy,
     *   - The `EThread::lock` is released while Event Thread is sleep.
     * When other threads try to acquire the `EThread::lock` of the target Event Thread:
     *   - Acquired, indicating that the target Event Thread is sleep,
     *   - Failed, indicating that the target Event Thread is busy.
     */
    ink_mutex_acquire(&EventQueueExternal.lock);
    this->execute_regular();
    ink_mutex_release(&EventQueueExternal.lock);
    break;
  }
  case DEDICATED: {
    break;
  }
  default:
    ink_assert(!"bad case value (execute)");
    break;
  } /* End switch */
  // coverity[missing_unlock]
}

EThread::Metrics::Slice &
EThread::Metrics::Slice::operator+=(Slice const &that)
{
  this->_events._max                = std::max(this->_events._max, that._events._max);
  this->_events._min                = std::min(this->_events._min, that._events._min);
  this->_events._total             += that._events._total;
  this->_duration._min              = std::min(this->_duration._min, that._duration._min);
  this->_duration._max              = std::max(this->_duration._max, that._duration._max);
  this->_count                     += that._count;
  this->_wait                      += that._wait;
  this->_duration._max_drain_queue  = std::max(this->_duration._max_drain_queue, that._duration._max_drain_queue);
  this->_duration._max_io_wait      = std::max(this->_duration._max_io_wait, that._duration._max_io_wait);
  this->_duration._max_io_work      = std::max(this->_duration._max_io_work, that._duration._max_io_work);
  return *this;
}

void
EThread::Metrics::summarize(Metrics &global)
{
  // Accumulate in local first so each sample only needs to be processed once,
  // not N_EVENT_TIMESCALES times.
  Slice sum;

  // Only summarize if there's no outstanding decay.
  if (0 == _decay_count) {
    global._loop_timing += _loop_timing;
    global._api_timing  += _api_timing;
  }

  // To avoid race conditions, we back up one from the current metric block. It's close enough
  // and won't be updated during the time this method runs so it should be thread safe.
  // The acquire fence ensures we see all the preceeding updates to the previous slice.
  Slice *current = current_slice.load(std::memory_order_acquire);
  if (current == nullptr) {
    // The thread has not entered its event loop yet, so it has no loop samples.
    // The stat sync can run during startup before every thread gets there.
    return;
  }
  Slice *slice = this->prev_slice(current);

  for (unsigned t = 0; t < N_TIMESCALES; ++t) {
    int count = SLICE_SAMPLE_COUNT[t];
    if (t > 0) {
      count -= SLICE_SAMPLE_COUNT[t - 1];
    }
    while (--count >= 0) {
      if (0 != slice->_duration._start) {
        sum += *slice;
      }
      slice = this->prev_slice(slice);
    }
    global._slice[t] += sum; // push out to return vector.
  }
}

void
EThread::set_tail_handler(LoopTailHandler *handler)
{
  ink_atomic_swap(&tail_cb, handler);
}

Event *
EThread::schedule_imm(Continuation *cont, int callback_event, void *cookie)
{
  Event *e = ::eventAllocator.alloc();

#ifdef ENABLE_EVENT_TRACKER
  e->set_location();
#endif

  e->callback_event = callback_event;
  e->cookie         = cookie;
  return schedule(e->init(cont, 0, 0));
}

Event *
EThread::schedule_at(Continuation *cont, ink_hrtime t, int callback_event, void *cookie)
{
  Event *e = ::eventAllocator.alloc();

#ifdef ENABLE_EVENT_TRACKER
  e->set_location();
#endif

  e->callback_event = callback_event;
  e->cookie         = cookie;
  return schedule(e->init(cont, t, 0));
}

Event *
EThread::schedule_in(Continuation *cont, ink_hrtime t, int callback_event, void *cookie)
{
  Event *e = ::eventAllocator.alloc();

#ifdef ENABLE_EVENT_TRACKER
  e->set_location();
#endif

  e->callback_event = callback_event;
  e->cookie         = cookie;
  return schedule(e->init(cont, ink_get_hrtime() + t, 0));
}

Event *
EThread::schedule_every(Continuation *cont, ink_hrtime t, int callback_event, void *cookie)
{
  Event *e = ::eventAllocator.alloc();

#ifdef ENABLE_EVENT_TRACKER
  e->set_location();
#endif

  e->callback_event = callback_event;
  e->cookie         = cookie;
  if (t < 0) {
    return schedule(e->init(cont, t, t));
  } else {
    return schedule(e->init(cont, ink_get_hrtime() + t, t));
  }
}

Event *
EThread::schedule(Event *e)
{
  e->ethread = this;
  if (tt != REGULAR) {
    ink_assert(tt == DEDICATED);
    return eventProcessor.schedule(e, ET_CALL);
  }
  if (e->continuation->mutex) {
    e->mutex = e->continuation->mutex;
  } else {
    e->mutex = e->continuation->mutex = e->ethread->mutex;
  }
  ink_assert(e->mutex.get());

  // Make sure client IP debugging works consistently
  // The continuation that gets scheduled later is not always the
  // client VC, it can be HttpCacheSM etc. so save the flags
  e->continuation->control_flags.set_flags(get_cont_flags().get_flags());

  if (e->ethread == this_ethread()) {
    EventQueueExternal.enqueue_local(e);
  } else {
    EventQueueExternal.enqueue(e);
  }

  return e;
}

Event *
EThread::schedule_imm_local(Continuation *cont, int callback_event, void *cookie)
{
  Event *e = EVENT_ALLOC(eventAllocator, this);

#ifdef ENABLE_EVENT_TRACKER
  e->set_location();
#endif

  e->callback_event = callback_event;
  e->cookie         = cookie;
  return schedule_local(e->init(cont, 0, 0));
}

Event *
EThread::schedule_at_local(Continuation *cont, ink_hrtime t, int callback_event, void *cookie)
{
  Event *e = EVENT_ALLOC(eventAllocator, this);

#ifdef ENABLE_EVENT_TRACKER
  e->set_location();
#endif

  e->callback_event = callback_event;
  e->cookie         = cookie;
  return schedule_local(e->init(cont, t, 0));
}

Event *
EThread::schedule_in_local(Continuation *cont, ink_hrtime t, int callback_event, void *cookie)
{
  Event *e = EVENT_ALLOC(eventAllocator, this);

#ifdef ENABLE_EVENT_TRACKER
  e->set_location();
#endif

  e->callback_event = callback_event;
  e->cookie         = cookie;
  return schedule_local(e->init(cont, ink_get_hrtime() + t, 0));
}

Event *
EThread::schedule_every_local(Continuation *cont, ink_hrtime t, int callback_event, void *cookie)
{
  Event *e = EVENT_ALLOC(eventAllocator, this);

#ifdef ENABLE_EVENT_TRACKER
  e->set_location();
#endif

  e->callback_event = callback_event;
  e->cookie         = cookie;
  if (t < 0) {
    return schedule_local(e->init(cont, t, t));
  } else {
    return schedule_local(e->init(cont, ink_get_hrtime() + t, t));
  }
}

Event *
EThread::schedule_local(Event *e)
{
  if (tt != REGULAR) {
    ink_assert(tt == DEDICATED);
    return eventProcessor.schedule(e, ET_CALL);
  }
  if (!e->mutex) {
    e->ethread = this;
    e->mutex   = e->continuation->mutex;
  } else {
    ink_assert(e->ethread == this);
  }
  e->globally_allocated = false;

  // Make sure client IP debugging works consistently
  // The continuation that gets scheduled later is not always the
  // client VC, it can be HttpCacheSM etc. so save the flags
  e->continuation->control_flags.set_flags(get_cont_flags().get_flags());

  // If you need to schedule an event from a different thread, use Ethread::schedule_imm/at/in/every functions
  ink_release_assert(this == this_ethread());

  EventQueueExternal.enqueue_local(e);
  return e;
}

Event *
EThread::schedule_spawn(Continuation *c, int ev, void *cookie)
{
  ink_assert(this != this_ethread()); // really broken to call this from the same thread.
  if (start_event) {
    free_event(start_event);
  }
  start_event          = EVENT_ALLOC(eventAllocator, this);
  start_event->ethread = this;
  start_event->mutex   = this->mutex;
  start_event->init(c);
  start_event->callback_event = ev;
  start_event->cookie         = cookie;
  return start_event;
}
