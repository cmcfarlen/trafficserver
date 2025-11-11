/** @file

Benchmark for timing functions - comparing ink_get_hrtime vs new cycle-based timing

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

#include "tscore/ink_hrtime.h"
#include "tsutil/Timing.h"

#define CATCH_CONFIG_ENABLE_BENCHMARKING
#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>

TEST_CASE("BenchTiming", "[bench][timing]")
{
  SECTION("Basic timing calls")
  {
    BENCHMARK("ink_get_hrtime")
    {
      return ink_get_hrtime();
    };

    BENCHMARK("ts::timing::detail::read_cycle")
    {
      return ts::timing::detail::read_cycle();
    };

    BENCHMARK("ts::timing::detail::read_cycle_ordered")
    {
      return ts::timing::detail::read_cycle_ordered();
    };

    BENCHMARK("ts::timing::detail::read_cycle_serialized")
    {
      return ts::timing::detail::read_cycle_serialized();
    };
  }

  SECTION("Timing with conversion to nanoseconds")
  {
    BENCHMARK("ink_get_hrtime (returns nanoseconds)")
    {
      return ink_get_hrtime();
    };

    BENCHMARK("read_cycle + cycles_to_nanoseconds")
    {
      uint64_t cycles = ts::timing::detail::read_cycle();
      return ts::timing::cycles_to_nanoseconds(cycles);
    };

    BENCHMARK("read_cycle_ordered + cycles_to_nanoseconds")
    {
      uint64_t cycles = ts::timing::detail::read_cycle_ordered();
      return ts::timing::cycles_to_nanoseconds(cycles);
    };

    BENCHMARK("read_cycle_serialized + cycles_to_nanoseconds")
    {
      auto result = ts::timing::detail::read_cycle_serialized();
      return ts::timing::cycles_to_nanoseconds(result.first);
    };
  }

  SECTION("Overhead of cycle frequency lookup")
  {
    BENCHMARK("cycle_frequency")
    {
      return ts::timing::detail::cycle_frequency();
    };
  }

  SECTION("Timing deltas - measuring elapsed time")
  {
    BENCHMARK("ink_get_hrtime delta")
    {
      ink_hrtime start = ink_get_hrtime();
      // Simulate some work
      volatile int x = 0;
      for (int i = 0; i < 10; i++) {
        x += i;
      }
      ink_hrtime end = ink_get_hrtime();
      return end - start;
    };

    BENCHMARK("read_cycle delta")
    {
      uint64_t start = ts::timing::detail::read_cycle();
      // Simulate some work
      volatile int x = 0;
      for (int i = 0; i < 10; i++) {
        x += i;
      }
      uint64_t end = ts::timing::detail::read_cycle();
      return end - start;
    };

    BENCHMARK("read_cycle_ordered delta")
    {
      uint64_t start = ts::timing::detail::read_cycle_ordered();
      // Simulate some work
      volatile int x = 0;
      for (int i = 0; i < 10; i++) {
        x += i;
      }
      uint64_t end = ts::timing::detail::read_cycle_ordered();
      return end - start;
    };
  }

  SECTION("Practical pattern: Two calls + conversion")
  {
    BENCHMARK("ink_get_hrtime() x2 (returns nanoseconds)")
    {
      ink_hrtime start = ink_get_hrtime();
      // Simulate some work
      volatile int x = 0;
      for (int i = 0; i < 10; i++) {
        x += i;
      }
      ink_hrtime end = ink_get_hrtime();
      return end - start; // Already in nanoseconds
    };

    BENCHMARK("read_cycle() x2 + cycles_to_nanoseconds()")
    {
      uint64_t start = ts::timing::detail::read_cycle();
      // Simulate some work
      volatile int x = 0;
      for (int i = 0; i < 10; i++) {
        x += i;
      }
      uint64_t end    = ts::timing::detail::read_cycle();
      uint64_t cycles = end - start;
      return ts::timing::cycles_to_nanoseconds(cycles);
    };

    BENCHMARK("read_cycle_ordered() x2 + cycles_to_nanoseconds()")
    {
      uint64_t start = ts::timing::detail::read_cycle_ordered();
      // Simulate some work
      volatile int x = 0;
      for (int i = 0; i < 10; i++) {
        x += i;
      }
      uint64_t end    = ts::timing::detail::read_cycle_ordered();
      uint64_t cycles = end - start;
      return ts::timing::cycles_to_nanoseconds(cycles);
    };
  }

  SECTION("CPU ID retrieval overhead")
  {
    BENCHMARK("read_cpu_id")
    {
      return ts::timing::detail::read_cpu_id();
    };

    BENCHMARK("read_cycle_serialized (cycles + cpu)")
    {
      auto result = ts::timing::detail::read_cycle_serialized();
      return result.first + result.second;
    };
  }
}

TEST_CASE("TimingAccuracy", "[timing][accuracy]")
{
  // Verify that cycle frequency detection is working
  uint64_t freq = ts::timing::detail::cycle_frequency();
  REQUIRE(freq > 0);
  INFO("Detected cycle frequency: " << freq << " Hz");

  // Test that conversions are reasonable
  uint64_t ns = ts::timing::cycles_to_nanoseconds(1000000);
  INFO("1,000,000 cycles = " << ns << " nanoseconds");
  REQUIRE(ns > 0);

  // Verify that ordering constraints work as expected
  uint64_t t1 = ts::timing::detail::read_cycle_ordered();
  uint64_t t2 = ts::timing::detail::read_cycle_ordered();
  REQUIRE(t2 >= t1);
}
