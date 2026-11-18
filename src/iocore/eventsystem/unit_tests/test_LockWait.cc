/** @file

  Catch2 unit tests for EThread::schedule_lock_wait_local: Events the event loop dispatches with a mutex held.

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

#define CATCH_CONFIG_THREAD_SAFE_ASSERTIONS
#include "inkevent_test_fixtures.h"

#include <atomic>
#include <chrono>
#include <thread>

using inkevent_test::AtomicFlag;
using inkevent_test::EventProcessorListener;

CATCH_REGISTER_LISTENER(EventProcessorListener)

namespace
{
/// Parks itself on @c target from an event thread, then records what the redispatch looked like.
class LockWaiter : public Continuation
{
public:
  LockWaiter(Ptr<ProxyMutex> target, bool lock_in_handler, bool cancel_after_park)
    : Continuation(new_ProxyMutex()),
      target(std::move(target)),
      lock_in_handler(lock_in_handler),
      cancel_after_park(cancel_after_park)
  {
    SET_HANDLER(&LockWaiter::park);
  }

  Ptr<ProxyMutex>  target;
  bool             lock_in_handler;
  bool             cancel_after_park;
  AtomicFlag       parked;
  AtomicFlag       dispatched;
  std::atomic<int> calls{0};
  bool             park_accepted{false};
  int              event_code{0};
  bool             held_on_entry{false};
  bool             adopt_on_entry{false};
  bool             handler_lock_ok{false};
  int              handler_depth{0};

private:
  int
  park(int /* event ATS_UNUSED */, void * /* data ATS_UNUSED */)
  {
    SET_HANDLER(&LockWaiter::run);
    Event *e      = this_ethread()->schedule_lock_wait_local(this, target.get(), EVENT_INTERVAL);
    park_accepted = e != nullptr;
    if (e != nullptr && cancel_after_park) {
      e->cancel_action();
    }
    parked.set();
    return EVENT_CONT;
  }

  int
  run(int event, void * /* data ATS_UNUSED */)
  {
    event_code     = event;
    held_on_entry  = target->thread_holding == this_ethread();
    adopt_on_entry = target->adopt_pending;
    if (lock_in_handler) {
      MUTEX_TRY_LOCK(lock, target, this_ethread());
      handler_lock_ok = lock.is_locked();
      handler_depth   = target->nthread_holding;
    }
    ++calls;
    dispatched.set();
    return EVENT_CONT;
  }
};

/// Wait until no thread holds @a m. The event loop releases an unadopted hold just after the handler returns, and
/// the Event keeps its own reference to the waiter's mutex, so the waiter can be freed once this returns.
bool
wait_until_free(ProxyMutex *m)
{
  auto const deadline = std::chrono::steady_clock::now() + inkevent_test::DEFAULT_TIMEOUT;
  while (std::chrono::steady_clock::now() < deadline) {
    if (__atomic_load_n(&m->thread_holding, __ATOMIC_ACQUIRE) == nullptr) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}
} // namespace

TEST_CASE("A lock-wait Event on a free mutex runs with the mutex held, and the handler's lock adopts the hold",
          "[inkevent][lockwait]")
{
  Ptr<ProxyMutex> target{new_ProxyMutex()};
  auto           *w = new LockWaiter(target, true, false);

  eventProcessor.schedule_imm(w, ET_CALL);

  REQUIRE(w->dispatched.wait_until_set());
  CHECK(w->park_accepted);
  CHECK(w->calls == 1);
  CHECK(w->event_code == EVENT_INTERVAL);
  CHECK(w->held_on_entry);
  CHECK(w->adopt_on_entry);
  CHECK(w->handler_lock_ok);
  CHECK(w->handler_depth == 1); // adopted, not nested
  REQUIRE(wait_until_free(target.get()));
  CHECK(target->nthread_holding == 0);
  CHECK_FALSE(target->adopt_pending);
  delete w;
}

TEST_CASE("A lock-wait Event on a held mutex is not dispatched until the mutex is released", "[inkevent][lockwait][multithread]")
{
  Ptr<ProxyMutex> target{new_ProxyMutex()};
  auto           *w = new LockWaiter(target, true, false);
  EThread        *t = this_ethread();

  MUTEX_TAKE_LOCK(target, t);
  eventProcessor.schedule_imm(w, ET_CALL);
  REQUIRE(w->parked.wait_until_set());

  // Several loop passes, each of which tries the mutex once.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_FALSE(w->dispatched.is_set());

  MUTEX_UNTAKE_LOCK(target, t);
  REQUIRE(w->dispatched.wait_until_set());
  CHECK(w->calls == 1);
  CHECK(w->held_on_entry);
  CHECK(w->handler_lock_ok);
  CHECK(w->handler_depth == 1);
  REQUIRE(wait_until_free(target.get()));
  delete w;
}

TEST_CASE("The event loop releases a lock-wait hold that the handler never adopted", "[inkevent][lockwait]")
{
  Ptr<ProxyMutex> target{new_ProxyMutex()};
  auto           *w = new LockWaiter(target, false, false);

  eventProcessor.schedule_imm(w, ET_CALL);

  REQUIRE(w->dispatched.wait_until_set());
  CHECK(w->held_on_entry);
  CHECK(w->adopt_on_entry);
  REQUIRE(wait_until_free(target.get()));
  CHECK(target->nthread_holding == 0);
  CHECK_FALSE(target->adopt_pending);
  delete w;
}

TEST_CASE("A cancelled lock-wait Event is dropped without running its handler or holding the mutex",
          "[inkevent][lockwait][multithread]")
{
  Ptr<ProxyMutex> target{new_ProxyMutex()};
  auto           *w = new LockWaiter(target, true, true);
  EThread        *t = this_ethread();

  MUTEX_TAKE_LOCK(target, t);
  eventProcessor.schedule_imm(w, ET_CALL);
  REQUIRE(w->parked.wait_until_set());
  MUTEX_UNTAKE_LOCK(target, t);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(w->park_accepted);
  CHECK(w->calls == 0);
  REQUIRE(wait_until_free(target.get()));
  CHECK_FALSE(target->adopt_pending);
  delete w;
}
