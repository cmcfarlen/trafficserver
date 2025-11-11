/** @file

Multi-threaded benchmark for timing functions - comparing scalability across thread counts

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

#include <atomic>
#include <thread>
#include <vector>
#include <iostream>
#include <iomanip>
#include <chrono>

// Number of iterations per thread
constexpr int ITERATIONS = 1000000;

struct BenchResult {
  int         threads;
  const char *method;
  double      total_time_ms;
  double      ops_per_second;
  double      ns_per_op;
};

void
print_header()
{
  std::cout << std::setw(10) << "Threads" << std::setw(30) << "Method" << std::setw(15) << "Time (ms)" << std::setw(20)
            << "Ops/sec (M)" << std::setw(15) << "ns/op" << std::endl;
  std::cout << std::string(90, '-') << std::endl;
}

void
print_result(const BenchResult &result)
{
  std::cout << std::setw(10) << result.threads << std::setw(30) << result.method << std::setw(15) << std::fixed
            << std::setprecision(2) << result.total_time_ms << std::setw(20) << std::fixed << std::setprecision(2)
            << result.ops_per_second << std::setw(15) << std::fixed << std::setprecision(2) << result.ns_per_op << std::endl;
}

// Benchmark ink_get_hrtime with multiple threads
BenchResult
bench_ink_get_hrtime(int num_threads)
{
  std::vector<std::thread> threads;
  std::atomic<uint64_t>    total_ops{0};

  auto start = std::chrono::high_resolution_clock::now();

  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&total_ops]() {
      for (int i = 0; i < ITERATIONS; ++i) {
        ink_hrtime t1 = ink_get_hrtime();
        ink_hrtime t2 = ink_get_hrtime();
        // Prevent optimization
        asm volatile("" : : "r,m"(t1) : "memory");
        asm volatile("" : : "r,m"(t2) : "memory");
      }
      total_ops += ITERATIONS;
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  auto   end      = std::chrono::high_resolution_clock::now();
  auto   duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
  double time_ms  = duration.count() / 1e6;

  uint64_t ops     = total_ops.load();
  double   ops_sec = (ops / (duration.count() / 1e9)) / 1e6; // millions of ops/sec
  double   ns_op   = duration.count() / static_cast<double>(ops);

  return BenchResult{num_threads, "ink_get_hrtime x2", time_ms, ops_sec, ns_op};
}

// Benchmark read_cycle with multiple threads
BenchResult
bench_read_cycle(int num_threads)
{
  std::vector<std::thread> threads;
  std::atomic<uint64_t>    total_ops{0};

  auto start = std::chrono::high_resolution_clock::now();

  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&total_ops]() {
      for (int i = 0; i < ITERATIONS; ++i) {
        uint64_t t1 = ts::timing::detail::read_cycle();
        uint64_t t2 = ts::timing::detail::read_cycle();
        uint64_t ns = ts::timing::cycles_to_nanoseconds(t2 - t1);
        // Prevent optimization
        asm volatile("" : : "r,m"(ns) : "memory");
      }
      total_ops += ITERATIONS;
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  auto   end      = std::chrono::high_resolution_clock::now();
  auto   duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
  double time_ms  = duration.count() / 1e6;

  uint64_t ops     = total_ops.load();
  double   ops_sec = (ops / (duration.count() / 1e9)) / 1e6;
  double   ns_op   = duration.count() / static_cast<double>(ops);

  return BenchResult{num_threads, "read_cycle x2 + convert", time_ms, ops_sec, ns_op};
}

// Benchmark read_cycle_ordered with multiple threads
BenchResult
bench_read_cycle_ordered(int num_threads)
{
  std::vector<std::thread> threads;
  std::atomic<uint64_t>    total_ops{0};

  auto start = std::chrono::high_resolution_clock::now();

  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&total_ops]() {
      for (int i = 0; i < ITERATIONS; ++i) {
        uint64_t t1 = ts::timing::detail::read_cycle_ordered();
        uint64_t t2 = ts::timing::detail::read_cycle_ordered();
        uint64_t ns = ts::timing::cycles_to_nanoseconds(t2 - t1);
        // Prevent optimization
        asm volatile("" : : "r,m"(ns) : "memory");
      }
      total_ops += ITERATIONS;
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  auto   end      = std::chrono::high_resolution_clock::now();
  auto   duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
  double time_ms  = duration.count() / 1e6;

  uint64_t ops     = total_ops.load();
  double   ops_sec = (ops / (duration.count() / 1e9)) / 1e6;
  double   ns_op   = duration.count() / static_cast<double>(ops);

  return BenchResult{num_threads, "read_cycle_ordered x2 + convert", time_ms, ops_sec, ns_op};
}

int
main(int /* argc */, char ** /* argv */)
{
  std::vector<int> thread_counts = {1, 2, 4, 8, 16};

  std::cout << "\nMulti-threaded Timing Benchmark" << std::endl;
  std::cout << "Each thread performs " << ITERATIONS << " iterations" << std::endl;
  std::cout << "Pattern: Two timing calls + conversion to nanoseconds\n" << std::endl;

  print_header();

  for (int num_threads : thread_counts) {
    // Benchmark each method
    auto result1 = bench_ink_get_hrtime(num_threads);
    print_result(result1);

    auto result2 = bench_read_cycle(num_threads);
    print_result(result2);

    auto result3 = bench_read_cycle_ordered(num_threads);
    print_result(result3);

    std::cout << std::endl;
  }

  return 0;
}
