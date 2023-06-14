/** @file

Simple benchmark for LogObject

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

/*

To run this, add this to Makefile.am in the proxy/logging directory

noinst_PROGRAMS = benchmark_LogObject
benchmark_LogObject_SOURCES = unit-tests/benchmark_LogObject.cc LogConfig.cc
benchmark_LogObject_CPPFLAGS = \
       $(AM_CPPFLAGS) \
       -I$(abs_top_srcdir)/tests/include
benchmark_LogObject_LDADD = \
       $(top_builddir)/src/tscore/libtscore.la \
       $(top_builddir)/src/tscpp/util/libtscpputil.la \
       $(top_builddir)/iocore/eventsystem/libinkevent.a \
       $(top_builddir)/proxy/logging/liblogging.a \
       $(top_builddir)/lib/records/librecords_p.a \
       $(top_builddir)/lib/records/librecords_lm.a \
       $(top_builddir)/proxy/logging/liblogging.a \
       $(top_builddir)/proxy/http/libhttp.a \
       $(top_builddir)/proxy/hdrs/libhdrs.a \
       $(top_builddir)/iocore/eventsystem/libinkevent.a \
       $(top_builddir)/proxy/shared/libdiagsconfig.a \
       @HWLOC_LIBS@

 */

#define CATCH_CONFIG_ENABLE_BENCHMARKING
#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "LogConfig.h"
#include "Log.h"
#include "DiagsConfig.h"
#include "records/I_RecLocal.h"
#include "tscore/I_Layout.h"
#include "I_Machine.h"

#include <thread>
#include <condition_variable>
#include <chrono>

AppVersionInfo appVersionInfo;
static char bind_stdout[512] = "";
static char bind_stderr[512] = "";

namespace notstd
{
struct barrier {
  int count;
  std::mutex m;
  std::condition_variable cv;

  barrier(int count) : count(count) {}

  void
  arrive_and_wait()
  {
    std::unique_lock lock{m};
    if (0 == --count) {
      cv.notify_all();
    } else {
      cv.wait(lock, [this] { return count == 0; });
    }
  }
};
} // namespace notstd

std::atomic<uint64_t> cas_misses(0);
void
test_logs(LogObject *logo, int thread_cnt)
{
  notstd::barrier barrier(thread_cnt);
  auto test_object = [&](LogObject *o) {
    EThread *me = new EThread;
    me->set_specific();
    me->set_event_type(ET_CALL);
    barrier.arrive_and_wait();

    std::string_view logline = "012345678901234567890123456789012345678901234567890";
    int total                = 0;
    while (total < Log::config->log_buffer_size * 100) {
      auto rc = o->log(nullptr, logline);
      if (rc != Log::LOG_OK) {
        throw std::runtime_error("Log failed");
      }
      total += logline.size();
    }

    cas_misses.fetch_add(o->get_local_cas_misses());
    o->reset_cas_misses();
  };

  REQUIRE(logo->writes_to_disk());
  REQUIRE(!logo->writes_to_pipe());

  std::vector<std::thread> threads;
  threads.reserve(thread_cnt);

  for (int i = 0; i < thread_cnt; ++i) {
    threads.emplace_back(test_object, logo);
  }
  for (int i = 0; i < thread_cnt; ++i) {
    threads[i].join();
  }
}

TEST_CASE("LogObject", "[proxy/logging]")
{
  ink_freelist_init_ops(true, true);
  init_buffer_allocators(0);
  Machine::init("benchmark_LogObject");

  Thread *main_thread = new EThread;
  main_thread->set_specific();

  // unused, but constructor must be called for side effects.
  new DiagsConfig("Server", "diags.log", "", "", false);

  diags()->set_std_output(StdStream::STDOUT, bind_stdout);
  diags()->set_std_output(StdStream::STDERR, bind_stderr);

  if (is_debug_tag_set("diags")) {
    diags()->dump();
  }
  Layout::create("/opt/ats");
  RecProcessInit();

  size_t stacksize;
  REC_ReadConfigInteger(stacksize, "proxy.config.thread.default.stacksize");
  eventProcessor.start(10, stacksize);

  Log::init(Log::NO_REMOTE_MANAGEMENT);

  LogFormat *fmt = MakeTextLogFormat();

  fmt->display(stdout);

  Log::config->format_list.add(fmt, false);
  Log::config->display(stdout);

  LogObject *slowo = new LogObject(Log::config, fmt, Log::config->logfile_dir, "atsbenchlogslow.txt", LOG_FILE_ASCII, "testheader",
                                   Log::NO_ROLLING, 1, 100, 100, 10, false, 0, 0, true, 0);
  LogObject *fasto = new LogObject(Log::config, fmt, Log::config->logfile_dir, "atsbenchlogfast.txt", LOG_FILE_ASCII, "testheader",
                                   Log::NO_ROLLING, 1, 100, 100, 10, false, 0, 0, true, 0, true);

  Log::config->log_object_manager.manage_object(slowo);
  Log::config->log_object_manager.manage_object(fasto);

  BENCHMARK("fast 1")
  {
    test_logs(fasto, 1);
  };
  BENCHMARK("slow 1")
  {
    test_logs(slowo, 1);
  };

  printf("CAS misses: %llu\n", cas_misses.load());
  cas_misses.store(0);

  BENCHMARK("fast 2")
  {
    test_logs(fasto, 2);
  };
  BENCHMARK("slow 2")
  {
    test_logs(slowo, 2);
  };
  printf("CAS misses: %llu\n", cas_misses.load());
  cas_misses.store(0);

  BENCHMARK("fast 4")
  {
    test_logs(fasto, 4);
  };
  BENCHMARK("slow 4")
  {
    test_logs(slowo, 4);
  };
  printf("CAS misses: %llu\n", cas_misses.load());
  cas_misses.store(0);

  BENCHMARK("fast 8")
  {
    test_logs(fasto, 8);
  };
  BENCHMARK("slow 8")
  {
    test_logs(slowo, 8);
  };
  printf("CAS misses: %llu\n", cas_misses.load());
  cas_misses.store(0);

  BENCHMARK("fast 16")
  {
    test_logs(fasto, 16);
  };
  BENCHMARK("slow 16")
  {
    test_logs(slowo, 16);
  };
  printf("CAS misses: %llu\n", cas_misses.load());
  cas_misses.store(0);

  BENCHMARK("fast 32")
  {
    test_logs(fasto, 32);
  };
  BENCHMARK("slow 32")
  {
    test_logs(slowo, 32);
  };
  printf("CAS misses: %llu\n", cas_misses.load());
  cas_misses.store(0);

  BENCHMARK("fast 64")
  {
    test_logs(fasto, 64);
  };
  BENCHMARK("slow 64")
  {
    test_logs(slowo, 64);
  };
  printf("CAS misses: %llu\n", cas_misses.load());
  cas_misses.store(0);
}
