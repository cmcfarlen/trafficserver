/** @file

Unit tests for ts::timing::Timing struct

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

#include "tsutil/Timing.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <chrono>
#include <ctime>
#include <sys/time.h>

// Local implementation of ink_get_hrtime to avoid dependency on tscore
namespace
{
using ink_hrtime = int64_t;

#define HRTIME_SECOND  (1000 * 1000 * 1000)
#define HRTIME_MSECOND (1000 * 1000)

static inline ink_hrtime
ink_hrtime_from_timespec(const struct timespec *ts)
{
  return ts->tv_sec * HRTIME_SECOND + ts->tv_nsec;
}

#if !defined(__FreeBSD__) && !defined(__APPLE__)
static inline ink_hrtime
ink_hrtime_from_timeval(const struct timeval *tv)
{
  return tv->tv_sec * HRTIME_SECOND + tv->tv_usec * 1000;
}
#endif

static inline ink_hrtime
ink_get_hrtime()
{
#if defined(__FreeBSD__) || defined(__APPLE__)
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ink_hrtime_from_timespec(&ts);
#else
  timeval tv;
  gettimeofday(&tv, nullptr);
  return ink_hrtime_from_timeval(&tv);
#endif
}
} // namespace

TEST_CASE("Timing basic functionality", "[timing]")
{
  ts::timing::Timing timer;

  SECTION("Timing is initialized")
  {
    // Timer should be initialized with a valid cycle count
    REQUIRE(timer.count > 0);
  }

  SECTION("elapsed() returns non-zero after work")
  {
    // Do some work
    volatile int sum = 0;
    for (int i = 0; i < 1000; i++) {
      sum += i;
    }

    uint64_t cycles = timer.elapsed();
    REQUIRE(cycles > 0);
  }

  SECTION("elapsed_time() returns reasonable nanoseconds")
  {
    // Do some work
    volatile int sum = 0;
    for (int i = 0; i < 1000; i++) {
      sum += i;
    }

    auto ns = timer.elapsed_time();
    REQUIRE(ns.count() > 0);
    // Should be less than 1 millisecond for this simple loop
    REQUIRE(ns.count() < 1'000'000);
  }

  SECTION("reset() updates the count")
  {
    uint64_t initial_count = timer.count;

    // Do some work
    volatile int sum = 0;
    for (int i = 0; i < 1000; i++) {
      sum += i;
    }

    timer.reset();
    uint64_t new_count = timer.count;

    REQUIRE(new_count > initial_count);
  }

  SECTION("elapsed() after reset returns smaller value")
  {
    // Do some work
    volatile int sum = 0;
    for (int i = 0; i < 10000; i++) {
      sum += i;
    }

    uint64_t elapsed1 = timer.elapsed();

    timer.reset();

    // Do less work
    for (int i = 0; i < 100; i++) {
      sum += i;
    }

    uint64_t elapsed2 = timer.elapsed();

    // elapsed2 should be much smaller since we reset and did less work
    REQUIRE(elapsed2 < elapsed1);
  }
}

TEST_CASE("Timing accuracy vs ink_get_hrtime", "[timing][accuracy]")
{
  SECTION("Short sleep comparison")
  {
    ts::timing::Timing timer;
    ink_hrtime         hrtime_start = ink_get_hrtime();

    // Sleep for a known duration
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto       cycle_elapsed  = timer.elapsed_time();
    ink_hrtime hrtime_elapsed = ink_get_hrtime() - hrtime_start;

    // Convert hrtime to nanoseconds (it's already in nanoseconds)
    auto hrtime_ns = hrtime_elapsed;

    INFO("Cycle-based: " << cycle_elapsed.count() << " ns");
    INFO("ink_get_hrtime: " << hrtime_ns << " ns");

    // Both should be around 10 milliseconds (10,000,000 ns)
    // Allow 30% tolerance for scheduling jitter (sleep is not precise)
    REQUIRE(cycle_elapsed.count() > 7'000'000);
    REQUIRE(cycle_elapsed.count() < 15'000'000);
    REQUIRE(hrtime_ns > 7'000'000);
    REQUIRE(hrtime_ns < 15'000'000);

    // The two measurements should be within 10% of each other
    double ratio = static_cast<double>(cycle_elapsed.count()) / static_cast<double>(hrtime_ns);
    INFO("Ratio: " << ratio);
    REQUIRE(ratio > 0.90);
    REQUIRE(ratio < 1.10);
  }

  SECTION("Multiple measurements correlation")
  {
    // Run multiple iterations and check that both methods are correlated
    const int iterations  = 10;
    double    total_ratio = 0.0;

    for (int i = 0; i < iterations; i++) {
      ts::timing::Timing timer;
      ink_hrtime         hrtime_start = ink_get_hrtime();

      // Variable amount of work
      volatile int sum = 0;
      for (int j = 0; j < (i + 1) * 1000; j++) {
        sum += j;
      }

      auto       cycle_elapsed  = timer.elapsed_time();
      ink_hrtime hrtime_elapsed = ink_get_hrtime() - hrtime_start;

      // Both should report non-zero time
      REQUIRE(cycle_elapsed.count() > 0);
      REQUIRE(hrtime_elapsed > 0);

      double ratio  = static_cast<double>(cycle_elapsed.count()) / static_cast<double>(hrtime_elapsed);
      total_ratio  += ratio;

      INFO("Iteration " << i << ": cycle=" << cycle_elapsed.count() << "ns, hrtime=" << hrtime_elapsed << "ns, ratio=" << ratio);
    }

    // Average ratio should be close to 1.0
    double avg_ratio = total_ratio / iterations;
    INFO("Average ratio: " << avg_ratio);
    REQUIRE(avg_ratio > 0.8);
    REQUIRE(avg_ratio < 1.2);
  }

  SECTION("Long duration comparison")
  {
    ts::timing::Timing timer;
    ink_hrtime         hrtime_start = ink_get_hrtime();

    // Sleep for a longer duration
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto       cycle_elapsed  = timer.elapsed_time();
    ink_hrtime hrtime_elapsed = ink_get_hrtime() - hrtime_start;

    INFO("Cycle-based: " << cycle_elapsed.count() << " ns");
    INFO("ink_get_hrtime: " << hrtime_elapsed << " ns");

    // Both should be around 100 milliseconds (100,000,000 ns)
    // Allow 10% tolerance
    REQUIRE(cycle_elapsed.count() > 90'000'000);
    REQUIRE(cycle_elapsed.count() < 110'000'000);
    REQUIRE(hrtime_elapsed > 90'000'000);
    REQUIRE(hrtime_elapsed < 110'000'000);

    // The two measurements should be within 5% of each other for longer durations
    double ratio = static_cast<double>(cycle_elapsed.count()) / static_cast<double>(hrtime_elapsed);
    INFO("Ratio: " << ratio);
    REQUIRE(ratio > 0.95);
    REQUIRE(ratio < 1.05);
  }
}

TEST_CASE("Timing edge cases", "[timing]")
{
  SECTION("Immediate elapsed() returns small value")
  {
    ts::timing::Timing timer;
    uint64_t           cycles = timer.elapsed();

    // Should be a very small number (just the overhead)
    // Typically less than 10000 cycles
    REQUIRE(cycles < 10'000);
  }

  SECTION("Multiple elapsed() calls without reset")
  {
    ts::timing::Timing timer;

    // Do some work
    volatile int sum = 0;
    for (int i = 0; i < 1000; i++) {
      sum += i;
    }

    uint64_t elapsed1 = timer.elapsed();

    // Do more work
    for (int i = 0; i < 1000; i++) {
      sum += i;
    }

    uint64_t elapsed2 = timer.elapsed();

    // Second elapsed should be larger since more time has passed
    // and count was not updated
    REQUIRE(elapsed2 > elapsed1);
  }

  SECTION("reset() can be called multiple times")
  {
    ts::timing::Timing timer;

    for (int i = 0; i < 5; i++) {
      uint64_t before_reset = timer.count;

      // Do some work to ensure time passes
      volatile int sum = 0;
      for (int j = 0; j < 1000; j++) {
        sum += j;
      }

      timer.reset();
      uint64_t after_reset = timer.count;

      // After doing work and resetting, count should have advanced
      REQUIRE(after_reset > before_reset);
    }
  }
}

TEST_CASE("Timing cycle frequency", "[timing]")
{
  SECTION("Cycle frequency is detected")
  {
    uint64_t freq = ts::timing::detail::cycle_frequency();
    REQUIRE(freq > 0);

    // Should be a reasonable CPU frequency (between 1 GHz and 10 GHz)
    REQUIRE(freq >= 1'000'000'000ULL);
    REQUIRE(freq < 10'000'000'000ULL);

    INFO("Detected cycle frequency: " << freq << " Hz");
  }

  SECTION("Cycle frequency is cached")
  {
    uint64_t freq1 = ts::timing::detail::cycle_frequency();
    uint64_t freq2 = ts::timing::detail::cycle_frequency();

    // Should return the same value (cached)
    REQUIRE(freq1 == freq2);
  }

  SECTION("cycles_to_nanoseconds is reasonable")
  {
    uint64_t freq = ts::timing::detail::cycle_frequency();

    // freq cycles should equal 1 second = 1,000,000,000 nanoseconds
    auto one_second = ts::timing::cycles_to_nanoseconds(freq);
    REQUIRE(one_second.count() == 1'000'000'000);

    // Half freq should equal 500ms
    auto half_second = ts::timing::cycles_to_nanoseconds(freq / 2);
    REQUIRE(half_second.count() == 500'000'000);
  }
}
