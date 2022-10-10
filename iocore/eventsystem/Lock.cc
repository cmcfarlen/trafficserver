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

/****************************************************************************

  Basic Locks for Threads



**************************************************************************/
#include "P_EventSystem.h"
#include "I_Event.h"
#include "tscore/Diags.h"

ClassAllocator<ProxyMutex> mutexAllocator("mutexAllocator");

void
lock_waiting(const SourceLocation &srcloc, const char *handler)
{
  if (is_diags_on("locks")) {
    char buf[128];
    fprintf(stderr, "WARNING: waiting on lock %s for %s\n", srcloc.str(buf, sizeof(buf)), handler ? handler : "UNKNOWN");
  }
}

void
lock_holding(const SourceLocation &srcloc, const char *handler)
{
  if (is_diags_on("locks")) {
    char buf[128];
    fprintf(stderr, "WARNING: holding lock %s too long for %s\n", srcloc.str(buf, sizeof(buf)), handler ? handler : "UNKNOWN");
  }
}

void
lock_taken(const SourceLocation &srcloc, const char *handler)
{
  if (is_diags_on("locks")) {
    char buf[128];
    fprintf(stderr, "WARNING: lock %s taken too many times for %s\n", srcloc.str(buf, sizeof(buf)), handler ? handler : "UNKNOWN");
  }
}

Event *&
EventLink::next_link(Event *e)
{
  return Event::Link_link::next_link(e);
}

Event *&
EventLink::prev_link(Event *e)
{
  return Event::Link_link::prev_link(e);
}

const Event *
EventLink::next_link(const Event *e)
{
  return Event::Link_link::next_link(e);
}

const Event *
EventLink::prev_link(const Event *e)
{
  return Event::Link_link::prev_link(e);
}

AsyncLockController::AsyncLockController(
#ifdef DEBUG
  const SourceLocation &location, const char *ahandler,
#endif
  Event *e, Ptr<ProxyMutex> &m)
  :
#ifdef DEBUG
    location(location),
    ahandler(ahandler),
#endif
    e(e),
    m(m),
    cookie(e->cookie),
    released(false)
{
}
AsyncLockController::~AsyncLockController()
{
  if (!released) {
    release_and_schedule_waiting();
  }
}

void
AsyncLockController::release_and_schedule_waiting()
{
  if (!released) {
    // Mutex_unlock will schedule a waiting event
    Mutex_unlock(m, this_ethread());
    released = true;
  }
}

static ClassAllocator<AsyncLockController> asyncLockControllerAllocator("AsyncLockController");

void
free_async_controller(AsyncLockController *c)
{
  asyncLockControllerAllocator.free(c);
}

bool
Mutex_lock_or_enqueue(
#ifdef DEBUG
  const SourceLocation &location, const char *ahandler,
#endif
  Ptr<ProxyMutex> &m, Event *e)
{
  ink_mutex_acquire(&m->q_mutex);
  auto *controller = asyncLockControllerAllocator.alloc(
#ifdef DEBUG
    location, ahandler,
#endif
    e, m);
  e->cookie      = static_cast<void *>(controller);
  bool is_locked = Mutex_trylock(location, ahandler, m, e->ethread);
  if (!is_locked) {
    std::printf("%p: lock_or_enqueue thread=%p holding=%p (%p) enqueueing intra-thread event\n", m.get(), this_ethread(),
                m.get()->thread_holding, e);
    m->queue.push(e);
  }
  ink_mutex_release(&m->q_mutex);
  return is_locked;
}

void
Mutex_adopt_lock_for_event(ProxyMutex *m, EThread *t, Event *e)
{
  // The cookie pointer in e is a AsyncLockController instance, so we need to replace the cookie pointer with the one
  // stored in the controller
  // maybe need to transfer the lock to another thread
  if (e->ethread != this_ethread()) {
    std::printf("%p: adopt_lock_for_event thread=%p target thread=%p (%p) adopting lock and scheduling\n", m, this_ethread(),
                e->ethread, e);
    m->thread_holding = nullptr;
    // set this flag so the schedule knows to handle the async controller
    e->holds_lock = true;
    ink_mutex_release(&m->the_mutex);
    e->ethread->schedule(e);
  } else {
    std::printf("%p: adopt_lock_for_event thread=%p (%p) dispatching next event\n", m, this_ethread(), e);
    t->process_event(e, CONTINUATION_EVENT_NONE); // use a "lock available" calling code?
  }
}

#ifdef LOCK_CONTENTION_PROFILING
void
ProxyMutex::print_lock_stats(int flag)
{
  if (flag) {
    if (total_acquires < 10)
      return;
    printf("Lock Stats (Dying):successful %d (%.2f%%), unsuccessful %d (%.2f%%) blocking %d \n", successful_nonblocking_acquires,
           (nonblocking_acquires > 0 ? successful_nonblocking_acquires * 100.0 / nonblocking_acquires : 0.0),
           unsuccessful_nonblocking_acquires,
           (nonblocking_acquires > 0 ? unsuccessful_nonblocking_acquires * 100.0 / nonblocking_acquires : 0.0), blocking_acquires);
    fflush(stdout);
  } else {
    if (!(total_acquires % 100)) {
      printf("Lock Stats (Alive):successful %d (%.2f%%), unsuccessful %d (%.2f%%) blocking %d \n", successful_nonblocking_acquires,
             (nonblocking_acquires > 0 ? successful_nonblocking_acquires * 100.0 / nonblocking_acquires : 0.0),
             unsuccessful_nonblocking_acquires,
             (nonblocking_acquires > 0 ? unsuccessful_nonblocking_acquires * 100.0 / nonblocking_acquires : 0.0),
             blocking_acquires);
      fflush(stdout);
    }
  }
}
#endif // LOCK_CONTENTION_PROFILING
