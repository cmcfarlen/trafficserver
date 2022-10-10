/** @file

  Catch based unit tests for EventSystem

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

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include <thread>
#include <chrono>

#include "I_EventSystem.h"
#include "tscore/I_Layout.h"
#include "tscore/TSSystemState.h"

#include "diags.i"

#define TEST_TIME_SECOND 60
#define TEST_THREADS 2

TEST_CASE("AsyncMutex", "[iocore]")
{
  static int count;
  static Ptr<ProxyMutex> the_mutex(new_ProxyMutex());
  static ink_hrtime time_start = Thread::get_hrtime_updated();

  struct mutex_user : public Continuation {
    mutex_user(ProxyMutex *m) : Continuation(m) { SET_HANDLER(&mutex_user::do_stuff); }

    int
    do_stuff(int /* event ATS_UNUSED */, Event * /* e ATS_UNUSED */)
    {
      ink_atomic_increment(&count, 1);

      EThread *e = this_ethread();
      std::printf("thread=%d (%p) count = %d\n", e->id, e, count);

      SET_HANDLER(&mutex_user::locked_stuff);
      e->schedule_with_lock(this, the_mutex);

      return 0;
    }

    int
    locked_stuff(int, Event *)
    {
      EThread *e = this_ethread();
      REQUIRE(the_mutex->thread_holding == e);

      ink_hrtime time_now = Thread::get_hrtime_updated();
      std::printf("thread=%d (%p) locked stuff = %d (%lld ms)\n", e->id, e, count, (time_now - time_start));
      using namespace std::chrono_literals;
      std::this_thread::sleep_for(10ms);

      // SET_HANDLER(&mutex_user::locked_stuff);
      // e->schedule_with_lock(this, the_mutex);

      return 0;
    }
  };

  struct process_killer : public Continuation {
    process_killer(ProxyMutex *m) : Continuation(m) { SET_HANDLER(&process_killer::kill_function); }

    int
    kill_function(int /* event ATS_UNUSED */, Event * /* e ATS_UNUSED */)
    {
      EThread *e = this_ethread();
      std::printf("thread=%d (%p) count is %d\n", e->id, e, count);

      REQUIRE(count > 0);
      REQUIRE(count <= TEST_TIME_SECOND * TEST_THREADS);

      TSSystemState::shut_down_event_system();

      return 0;
    }
  };

  process_killer *killer = new process_killer(new_ProxyMutex());
  eventProcessor.schedule_in(killer, HRTIME_SECONDS(5));
  eventProcessor.schedule_in(new mutex_user(new_ProxyMutex()), HRTIME_MSECONDS(1));
  eventProcessor.schedule_in(new mutex_user(new_ProxyMutex()), HRTIME_MSECONDS(1));

  while (!TSSystemState::is_event_system_shut_down()) {
    sleep(1);
  }
}

struct EventProcessorListener : Catch::TestEventListenerBase {
  using TestEventListenerBase::TestEventListenerBase;

  void
  testRunStarting(Catch::TestRunInfo const &testRunInfo) override
  {
    Layout::create();
    init_diags("", nullptr);
    RecProcessInit(RECM_STAND_ALONE);

    ink_event_system_init(EVENT_SYSTEM_MODULE_PUBLIC_VERSION);
    eventProcessor.start(TEST_THREADS, 1048576); // Hardcoded stacksize at 1MB

    EThread *main_thread = new EThread;
    main_thread->set_specific();
  }
};

CATCH_REGISTER_LISTENER(EventProcessorListener);
