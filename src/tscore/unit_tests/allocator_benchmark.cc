/** @file

  Micro Benchmark tool for Allocator/ClassAllocator - requires Catch2 v2.9.0+

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

#define CATCH_CONFIG_ENABLE_BENCHMARKING
#define CATCH_CONFIG_RUNNER

#include <thread>
#include <array>
#include <vector>
#include "catch.hpp"

#include "tscore/Allocator.h"

// options
int nthreads = 1;

struct TestStruct {
  std::array<char, 64> data = {1, 0};
};

TEST_CASE("allocator", "")
{
  auto alloc = ClassAllocator<TestStruct>("tester");

  auto *p = alloc.alloc();

  REQUIRE(p->data[0] == 1);
  REQUIRE(p->data[1] == 0);

  p->data[0] = 42;
  alloc.free(p);

  p = alloc.alloc();
  REQUIRE(p->data[0] == 1);
  alloc.free(p);
}

template <typename A>
int
run_benchmark()
{
  using TestAllocator = ClassAllocator<TestStruct, true, A>;

  TestAllocator a("test");
  auto small  = A("small", 4096);
  auto medium = A("medium", 4096 * 20);
  auto large  = A("large", 4096 * 100);

  auto f = [](TestAllocator &a, A &small, A &medium, A &large) {
    for (int i = 0; i < 10000; ++i) {
      auto *p = a.alloc_void();
      a.free_void(p);
    }

    for (int i = 0; i < 1000; ++i) {
      auto *p = small.alloc_void();
      small.free_void(p);
    }
    for (int i = 0; i < 500; ++i) {
      auto *p = medium.alloc_void();
      medium.free_void(p);
    }
    for (int i = 0; i < 100; ++i) {
      auto *p = large.alloc_void();
      large.free_void(p);
    }
  };

  auto threads = std::vector<std::thread>();
  threads.reserve(nthreads);
  for (int i = 0; i < nthreads; ++i) {
    threads.emplace_back(f, std::ref(a), std::ref(small), std::ref(medium), std::ref(large));
  }

  for (int i = 0; i < nthreads; ++i) {
    threads[i].join();
  }

  return 0;
}

TEST_CASE("allocate bench", "")
{
  BENCHMARK("freelistallocator") { return run_benchmark<FreelistAllocator>(); };
  BENCHMARK("mallocallocator") { return run_benchmark<MallocAllocator>(); };
}
template <typename A>
int
run_benchmark_advice()
{
  A alloc("advice", 64 * 1024);

  int advice = 0;
#ifdef MADV_DONTDUMP // This should only exist on Linux 3.4 and higher.
  advice = MADV_DONTDUMP;
#else
  advice = MADV_SEQUENTIAL;
#endif
  alloc.re_init("advice", 64 * 1024, 32, 8, advice);

  for (int i = 0; i < 1000; ++i) {
    auto *p = alloc.alloc_void();
    alloc.free_void(p);
  }
  return 0;
}

TEST_CASE("allocate advice", "")
{
  BENCHMARK("freelistallocator") { return run_benchmark_advice<FreelistAllocator>(); };
  BENCHMARK("mallocallocator") { return run_benchmark_advice<MallocAllocator>(); };
}

int
main(int argc, char *argv[])
{
  Catch::Session session;

  using namespace Catch::clara;

  /*
  bool opt_enable_hugepage = false;
   */

  auto cli = session.cli() | Opt(nthreads, "n")["--ts-nthreads"]("number of threads\n"
                                                                 "(default: 1)")
    /*
    Opt(affinity, "type")["--ts-affinity"]("thread affinity type [0-4]\n"
                                           "0 = HWLOC_OBJ_MACHINE (default)\n"
                                           "1 = HWLOC_OBJ_NODE\n"
                                           "2 = HWLOC_OBJ_SOCKET\n"
                                           "3 = HWLOC_OBJ_CORE\n"
                                           "4 = HWLOC_OBJ_PU") |
    Opt(nloop, "n")["--ts-nloop"]("number of loop\n"
                                  "(default: 1000000)") |
    Opt(nthreads, "n")["--ts-nthreads"]("number of threads\n"
                                        "(default: 1)") |
    Opt(opt_enable_hugepage, "yes|no")["--ts-hugepage"]("enable hugepage\n"
                                                        "(default: no)") |
    Opt(thread_assiging_order, "n")["--ts-thread-order"]("thread assiging order [0-1]\n"
                                                         "0: use both of sibling of hyper-thread first (default)\n"
                                                         "1: use a side of sibling of hyper-thread first") |
    Opt(debug_enabled, "yes|no")["--ts-debug"]("enable debuge mode\n")
     */
    ;

  session.cli(cli);

  int returnCode = session.applyCommandLine(argc, argv);
  if (returnCode != 0) {
    return returnCode;
  }

  /*
  if (debug_enabled) {
    std::cout << "nloop = " << nloop << std::endl;

    if (opt_enable_hugepage) {
      std::cout << "hugepage enabled";
#ifdef MAP_HUGETLB
      ats_hugepage_init(true);
      std::cout << " ats_pagesize=" << ats_pagesize();
      std::cout << " ats_hugepage_size=" << ats_hugepage_size();
      std::cout << std::endl;
#else
      std::cout << "MAP_HUGETLB not defined" << std::endl;
#endif
    }
  }
   */

  return session.run();
}