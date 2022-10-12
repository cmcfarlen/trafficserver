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

#include <array>
#include "catch.hpp"

#include "tscore/Allocator.h"

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

int
main(int argc, char *argv[])
{
  Catch::Session session;

  using namespace Catch::clara;

  /*
  bool opt_enable_hugepage = false;

  auto cli = session.cli() |
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
             Opt(debug_enabled, "yes|no")["--ts-debug"]("enable debuge mode\n");
*/
  auto cli = session.cli();

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