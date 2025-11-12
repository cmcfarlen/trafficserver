/*
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

#pragma once

#include <chrono>
#include <cstdint>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#include <cpuid.h>
#elif defined(__aarch64__)
#if defined(__clang__)
#include <arm_acle.h>
#endif
#else
#error "Timing.h: Only x86-64 and AArch64 supports timing."
#endif

namespace ts
{
/** utility for timing code
 *
 */
namespace timing
{
  namespace detail
  {
#if defined(__x86_64__) || defined(_M_X64)
    [[gnu::always_inline]] static inline uint64_t
    read_cycle() noexcept
    {
      return __rdtsc();
    }

    [[gnu::always_inline]] static inline uint64_t
    read_cycle_ordered() noexcept
    {
      uint64_t tsc;
      __asm__ volatile("lfence\n\t"
                       "rdtsc"
                       : "=A"(tsc)
                       :
                       : "memory");
      return tsc;
    }

    [[gnu::always_inline]] static inline std::pair<uint64_t, unsigned int>
    read_cycle_serialized() noexcept
    {
      unsigned int cpu;
      uint64_t     cycles = __rdtscp(&cpu);
      return {cycles, cpu};
    }

    [[gnu::always_inline]] static inline unsigned int
    read_cpu_id() noexcept
    {
      unsigned int aux;
      (void)__rdtscp(&aux);
      return aux;
    }
#elif defined(__aarch64__)
    [[gnu::always_inline]] static inline uint64_t
    read_cycle() noexcept
    {
      uint64_t val;
      __asm__ volatile("isb\n\t"
                       "mrs %0, CNTVCT_EL0\n\t"
                       "isb"
                       : "=r"(val)
                       :
                       : "memory");
      return val;
    }

    [[gnu::always_inline]] static inline uint64_t
    read_cycle_ordered() noexcept
    {
      return read_cycle(); // arm is already ordered
    }

    [[gnu::always_inline]] static inline std::pair<uint64_t, unsigned int>
    read_cycle_serialized() noexcept
    {
      // MPIDR_EL1 is a privileged register that requires EL1 (kernel mode)
      // User space cannot read it, so we return 0 for the CPU ID
      uint64_t cycles;
      __asm__ volatile("isb\n\t"
                       "mrs %0, CNTVCT_EL0\n\t"
                       "isb"
                       : "=r"(cycles)
                       :
                       : "memory");
      return {cycles, 0};
    }

    [[gnu::always_inline]] static inline unsigned int
    read_cpu_id() noexcept
    {
      // MPIDR_EL1 is a privileged register that requires EL1 (kernel mode)
      // User space cannot read it, so we return 0
      return 0;
    }
#endif // architecture
    static uint64_t cycles_per_second = 0;

    [[gnu::always_inline]] static inline uint64_t
    cycle_frequency() noexcept
    {
      if (cycles_per_second != 0) {
        return cycles_per_second;
      }

#if defined(__x86_64__) || defined(_M_X64)
      // Use CPUID to get nominal frequency (in 100 MHz units)
      uint32_t eax, ebx, ecx, edx;
      __cpuid_count(0x15, 0, eax, ebx, ecx, edx);
      if (ebx != 0 && ecx != 0) {
        // TSC frequency = (ECX * EBX) / EAX Hz
        cycles_per_second = ((uint64_t)ecx * ebx) / eax;
      } else {
        // Fallback: assume 2.0 GHz (common safe default)
        cycles_per_second = 2'000'000'000ULL;
      }

#elif defined(__aarch64__)
      // Read CNTFRQ_EL0 – fixed frequency of the counter
      uint64_t freq;
      __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
      cycles_per_second = freq;
#endif

      return cycles_per_second;
    }

  } // namespace detail

  [[gnu::always_inline]] static inline std::chrono::nanoseconds
  cycles_to_nanoseconds(uint64_t cycles) noexcept
  {
    const uint64_t freq = detail::cycle_frequency();
    // (cycles * 1e9) / freq
    return std::chrono::nanoseconds((cycles * 1'000'000'000ULL) / freq);
  }

  struct Timing {
    uint64_t count;

    Timing() { reset(); }

    void
    reset()
    {
      count = detail::read_cycle_ordered();
    }

    /**
     * Returns the cycle count since the last call to reset.
     */
    uint64_t
    elapsed()
    {
      auto current = detail::read_cycle_ordered();
      return current - count;
    }

    std::chrono::nanoseconds
    elapsed_time()
    {
      return cycles_to_nanoseconds(elapsed());
    }
  };
} // namespace timing

} // namespace ts
