# InactivityCop benchmark — baseline of record

Baseline measurements taken immediately before the `InactivityCop` timer-wheel
refactor. Four later checkpoints are appended to this file, so the tables and
the reading instructions below need to stay interpretable by someone who has
never seen this work.

## What is being measured, and why

The benchmark lives in `src/iocore/net/unit_tests/benchmark_InactivityCop.cc`
and is wired into the `test_net` Catch2 target, tagged `[!benchmark]` so a
normal `ctest` run skips it.

Each measurement times **one `InactivityCop::check_inactivity()` call** against
`N` mock `NetEvent`s registered on a hand-built `NetHandler`. The call is driven
synchronously on the `test_net` Catch2 thread — there is no event loop, no
scheduling, and no real socket. In production this same call runs once per
second on every ATS network thread; each run drains `NetHandler::cop_list`,
checks every connection for an expired timeout, and then refills the list by
walking all of `NetHandler::open_list`. That refill is an unconditional O(N)
sweep over every open connection on the thread, and it is the cost the timer
wheel is meant to remove.

The harness deliberately isolates timeout dispatch and excludes:

- **Real I/O.** Mocks never touch a file descriptor.
- **Queue management.** `NetHandler::config.max_connections_in`,
  `config.max_requests_in`, `max_connections_per_thread_in`, and
  `max_requests_per_thread_in` are all set to 0, so the connection-limit and
  request-limit paths early-return and contribute nothing.
- **Active timeouts.** No scenario ever sets one, so only the inactivity path
  is exercised.

Five scenarios run at N = 1000, 10000, 100000:

| scenario | what it sets up |
| --- | --- |
| `idle` | all connections idle, no deadline expired |
| `keepalive` | keepalive deadline value instead of the default inactivity deadline |
| `mass_expiry` | every connection's deadline has expired — N callbacks per run |
| `churn` | 1% of connections expire per run |
| `lock_contention` | 10% of mutex acquisitions fail, exercising the retry path |

The counter columns are instrumentation on the cop itself:

- `get_mutex/run` — mutex acquisitions attempted per `check_inactivity()` call.
- `get_thread/run` — `NetEvent`-to-thread touches per call; this is the
  `open_list` refill walk.
- `cb/run` — timeout callbacks actually dispatched.
- `lockfail/run` — failed mutex acquisitions.

## How to reproduce

Optimized (primary) build:

```
cmake --preset mydev -B build-bench -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-bench --target test_net
./build-bench/src/iocore/net/test_net "[inactivity_cop]"
```

Debug (secondary) build:

```
cmake --preset mydev            # configures build-mydev, CMAKE_BUILD_TYPE=Debug
cmake --build build-mydev --target test_net
./build-mydev/src/iocore/net/test_net "[inactivity_cop]"
```

`CMakeUserPresets.json` is gitignored and must not be committed. Note that
`cmake/proxy-verifier.cmake` performs an unconditional `file(DOWNLOAD ...)`
into the git *common* dir (`.git/proxy-verifier/proxy-verifier.tar.gz`), which
is shared with the main checkout; a cached tarball with the hash in
`tests/proxy-verifier-checksum.txt` makes configure skip it. Verify that file
is intact before configuring in a network-restricted environment.

## Environment

| | |
| --- | --- |
| host | `pebs.local` |
| CPU | Apple M4 Max, 16 cores, 16 MiB L2 (performance cluster) |
| RAM | 128 GiB |
| OS | macOS 26.6.2 (build 25G83), arm64 |
| compiler | Apple clang 21.0.0 (`clang-2100.1.1.101`), target `arm64-apple-darwin25.6.0`, `/usr/bin/c++` |
| commit | `59f6d87190` |

Build flags actually used, from each `CMakeCache.txt`:

| build dir | `CMAKE_BUILD_TYPE` | flags | sanitizers |
| --- | --- | --- | --- |
| `build-bench` | `RelWithDebInfo` | `-O3 -g -DNDEBUG` | none (`ENABLE_ASAN=FALSE`, `ENABLE_TSAN=OFF`) |
| `build-mydev` | `Debug` | `-g` (i.e. `-O0`) | none (`ENABLE_ASAN=FALSE`) |

Two notes on the flags, because they contradict some earlier assumptions:

- The `mydev` preset sets `ENABLE_ASAN=false`, so **neither** build has
  AddressSanitizer, despite what `CLAUDE.md` says about the preset. Both
  numbers below are sanitizer-free.
- The preset's `CXXFLAGS` entry is a *cache variable*, not an environment
  variable, so CMake reports it as unused and none of
  `-fPIE -fstack-protector -fno-omit-frame-pointer -flto=thin -stdlib=libc++`
  is applied. In particular there is **no LTO** in either build.

`-DNDEBUG` in the optimized build disables `ink_assert` / `debug_assert`.
Catch2 `CHECK`s still run: all 84 assertions across 5 test cases passed in every
run of both builds.

Benchmark parameters, verbatim from the `[provenance]` and `[sizes]` lines:

```
[provenance] commit=59f6d87190 build=release host=pebs.local
[provenance] N_VALUES=1000,10000,100000 MOCK_FOOTPRINT_BYTES=1584 SAMPLE_RUNS=25 WARMUP_RUNS=2 shuffle_seed=0xC0FFEE
[sizes] sizeof(MockNetEvent)=440 sizeof(PaddedMock)=1584 sizeof(UnixNetVConnection)=1560 (frozen footprint=1584)
[sizes] NOTE: live sizeof(UnixNetVConnection) differs from the frozen footprint; numbers are not directly comparable to the 300f40c9 baseline until MOCK_FOOTPRINT_BYTES is deliberately updated
```

```
[provenance] commit=59f6d87190 build=debug host=pebs.local
[provenance] N_VALUES=1000,10000,100000 MOCK_FOOTPRINT_BYTES=1584 SAMPLE_RUNS=25 WARMUP_RUNS=2 shuffle_seed=0xC0FFEE
[sizes] sizeof(MockNetEvent)=456 sizeof(PaddedMock)=1584 sizeof(UnixNetVConnection)=1584 (frozen footprint=1584)
```

The `build=release` token in the provenance line means "`NDEBUG` was defined";
the actual build type is `RelWithDebInfo` as configured above. The mock
footprint is frozen at 1584 bytes in both builds, so the working set at
N=100000 is 158.4 MB (151 MiB) either way. Note that live
`sizeof(UnixNetVConnection)` is 1560 under `-O3 -DNDEBUG` and 1584 under
`-O0`; the frozen footprint happens to match the Debug figure exactly and
overshoots the optimized one by 24 bytes.

## Baseline: RelWithDebInfo (PRIMARY)

`build-bench`, `-O3 -g -DNDEBUG`, commit `59f6d87190`. Run 1 of 3, verbatim.

```
scenario                 N    mean_ms     min_ms     max_ms      ns/conn  get_mutex/run get_thread/run     cb/run lockfail/run
--------                 -    -------     ------     ------      -------  ------------- --------------     ------ ------------
idle                  1000     0.0147     0.0132     0.0172        14.71           1000           1000          0            0
idle                 10000     0.2044     0.2007     0.2121        20.44          10000          10000          0            0
idle                100000    13.7995    11.6975    15.7157       138.00         100000         100000          0            0

keepalive             1000     0.0228     0.0222     0.0250        22.83           1000           1000          0            0
keepalive            10000     0.2921     0.2872     0.3202        29.21          10000          10000          0            0
keepalive           100000    12.5313    10.9269    16.3519       125.31         100000         100000          0            0

mass_expiry           1000     0.0154     0.0142     0.0160        15.43           1000           1000       1000            0
mass_expiry          10000     0.2127     0.2075     0.2274        21.27          10000          10000      10000            0
mass_expiry         100000    14.0547    11.6085    16.8944       140.55         100000         100000     100000            0

churn                 1000     0.0146     0.0138     0.0153        14.58           1000           1000         10            0
churn                10000     0.2128     0.2032     0.2447        21.28          10000          10000        100            0
churn               100000    13.1699    11.5057    14.8192       131.70         100000         100000       1000            0

lock_contention       1000     0.0169     0.0145     0.0300        16.92           1000           1000          0          100
lock_contention      10000     0.2233     0.1997     0.2825        22.33          10000          10000          0         1000
lock_contention     100000    14.3858    12.7636    17.0316       143.86         100000         100000          0        10000
```

The `keepalive` N=1000 and N=10000 figures in this particular run are the
highest of the three runs (0.0228 / 0.2921 vs ~0.014 / ~0.20 elsewhere); treat
them as noise, not as a real keepalive-specific cost. See the noise floor
section.

## Baseline: Debug (SECONDARY)

`build-mydev`, `-g` / `-O0`, commit `59f6d87190`. Single run. Useful only for
interpreting the earlier Debug-only numbers — not quotable as absolute cost.

```
scenario                 N    mean_ms     min_ms     max_ms      ns/conn  get_mutex/run get_thread/run     cb/run lockfail/run
--------                 -    -------     ------     ------      -------  ------------- --------------     ------ ------------
idle                  1000     0.0976     0.0934     0.1050        97.58           1000           1000          0            0
idle                 10000     1.0639     1.0382     1.1760       106.39          10000          10000          0            0
idle                100000    25.0966    23.4160    26.7311       250.97         100000         100000          0            0

keepalive             1000     0.1015     0.0948     0.1306       101.46           1000           1000          0            0
keepalive            10000     1.0496     0.9636     1.1651       104.96          10000          10000          0            0
keepalive           100000    26.0311    24.7734    28.2952       260.31         100000         100000          0            0

mass_expiry           1000     0.1114     0.1035     0.1204       111.41           1000           1000       1000            0
mass_expiry          10000     1.1664     1.1448     1.1906       116.64          10000          10000      10000            0
mass_expiry         100000    26.7506    25.7464    28.7278       267.51         100000         100000     100000            0

churn                 1000     0.1053     0.0947     0.1254       105.28           1000           1000         10            0
churn                10000     1.0598     0.9737     1.0987       105.98          10000          10000        100            0
churn               100000    25.2768    23.8423    27.3578       252.77         100000         100000       1000            0

lock_contention       1000     0.0987     0.0946     0.1137        98.67           1000           1000          0          100
lock_contention      10000     1.0820     0.9731     1.3264       108.20          10000          10000          0         1000
lock_contention     100000    25.6837    23.5824    29.0057       256.84         100000         100000          0        10000
```

The counter columns are byte-identical between the two builds, as they should
be — they count work, not time.

### Debug / RelWithDebInfo ratio

Using `idle` as the reference scenario:

| N | Debug ns/conn | RelWithDebInfo ns/conn | ratio |
| --- | --- | --- | --- |
| 1000 | 97.6 | ~13.9 | 7.0x |
| 10000 | 106.4 | ~19.7 | 5.4x |
| 100000 | 251.0 | ~138.5 | 1.8x |

`-O0` inflates the small-N numbers by 5-7x but only 1.8x at N=100000, because
at N=100000 the dominant cost is memory stalls, which the optimizer cannot
remove. This is exactly why the Debug numbers were not quotable, and it is also
why the Debug numbers *understated* how sharp the large-N knee is.

## Super-linear knee at N=100000 — reproduces, and is much sharper optimized

The Debug numbers were super-linear relative to N=10000. The optimized build
shows the **same knee, dramatically more pronounced**:

| build | `idle` mean at N=10000 | `idle` mean at N=100000 | wall-time factor for 10x N | ns/conn growth |
| --- | --- | --- | --- | --- |
| Debug | 1.064 ms | 25.10 ms | 23.6x | 2.4x |
| RelWithDebInfo | ~0.197 ms | ~13.85 ms | **~70x** | **~7.0x** |

This is consistent with the working set: 100000 x 1584 bytes = 151 MiB, far
past the 16 MiB L2 on this machine, whereas 10000 connections is 15.8 MiB and
roughly fits. At N=1000 (1.6 MiB) the sweep is essentially cache-resident.

This observation is load-bearing for the refactor. It means the cost the timer
wheel must beat **grows faster than linearly** at realistic connection counts,
because the `open_list` refill walk is a pointer chase across a working set
that does not fit in cache. Removing the walk should therefore win more than a
naive O(N) -> O(expired) accounting suggests. It also means any future
measurement that only looks at N=1000 and N=10000 will badly understate the
benefit.

## Noise floor

Three consecutive RelWithDebInfo runs. `mean_ms` per run:

| scenario | N | run 1 | run 2 | run 3 | spread | spread / mean |
| --- | --- | --- | --- | --- | --- | --- |
| idle | 1000 | 0.0147 | 0.0141 | 0.0129 | 0.0018 | 13% |
| idle | 10000 | 0.2044 | 0.1976 | 0.1894 | 0.0150 | 7.6% |
| idle | 100000 | 13.7995 | 13.6736 | 14.0785 | 0.405 | 2.9% |
| keepalive | 1000 | 0.0228 | 0.0146 | 0.0133 | 0.0095 | 57% |
| keepalive | 10000 | 0.2921 | 0.2120 | 0.1866 | 0.1055 | 45% |
| keepalive | 100000 | 12.5313 | 12.2277 | 12.0798 | 0.452 | 3.7% |
| mass_expiry | 1000 | 0.0154 | 0.0152 | 0.0170 | 0.0018 | 11% |
| mass_expiry | 10000 | 0.2127 | 0.2093 | 0.2524 | 0.0431 | 19% |
| mass_expiry | 100000 | 14.0547 | 13.9299 | 15.2171 | 1.287 | 8.9% |
| churn | 1000 | 0.0146 | 0.0148 | 0.0145 | 0.0003 | 2.1% |
| churn | 10000 | 0.2128 | 0.2013 | 0.2027 | 0.0115 | 5.5% |
| churn | 100000 | 13.1699 | 13.3897 | 14.9751 | 1.805 | 13% |
| lock_contention | 1000 | 0.0169 | 0.0163 | 0.0155 | 0.0014 | 8.5% |
| lock_contention | 10000 | 0.2233 | 0.2015 | 0.2089 | 0.0218 | 10% |
| lock_contention | 100000 | 14.3858 | 13.5376 | 13.7478 | 0.848 | 6.1% |

Within a *single* run at N=100000 the min/max spread across the 25 samples is
larger still: 3-7 ms on a ~14 ms mean, e.g. `idle` run 3 spanned
12.18-19.63 ms (53% of the mean). The Debug build's within-run spread at
N=100000 was proportionally tighter (23.4-26.7 ms on a 25 ms mean, 13%),
because its larger fixed per-connection cost dilutes the same absolute memory
jitter.

**Assessment, stated plainly:** the noise floor on wall-clock time is
approximately **10% at N=1000 and N=10000, and 5-15% at N=100000** run to run.
`keepalive` at small N was the worst offender at up to 57% across runs. This
means:

- A change smaller than about 15% in wall-clock time is **not measurable** here
  without many more runs than three. Do not claim a 5% or 10% improvement from
  this benchmark.
- The refactor is expected to move the large-N numbers by far more than 15%
  (the timer wheel removes the entire O(N) walk), so the wall-clock signal
  should still be unambiguous for the change we actually care about.
- For anything subtler, rely on the counter columns, which are exact and have
  zero run-to-run variance.

## How to read these numbers across checkpoints

1. **Wall-clock columns (`mean_ms`, `min_ms`, `max_ms`, `ns/conn`) are only
   comparable within the same build type on the same machine.** Never compare a
   Debug number to a RelWithDebInfo one, and never compare across hosts. Every
   checkpoint below must record its build type.
2. **The touch counts are the primary signal.** `get_mutex/run` and
   `get_thread/run` are hardware-independent, deterministic, and immune to the
   noise floor above. A checkpoint that moves them has definitely changed the
   algorithm; a checkpoint that only moves wall time by 10% has probably
   changed nothing.
3. **`get_thread/run` should collapse toward zero** when the timer wheel
   replaces the `open_list` refill walk. At baseline it equals N in every
   scenario — that *is* the O(N) sweep. If it still equals N after the
   switchover, the walk was not actually removed.
4. **`get_mutex/run` should fall** when the deadline check moves ahead of the
   lock (Phase 1), because connections that are not due no longer take a mutex.
   At baseline it equals N in every scenario, including `idle` where zero
   callbacks fire.
5. `cb/run` should stay fixed for a given scenario across all checkpoints. It
   describes the workload, not the implementation. If it changes, either the
   scenario setup drifted or the refactor changed which connections time out —
   both are bugs worth chasing.

## Known limitations

Carried forward honestly; these apply to every checkpoint in this file.

- **`keepalive` is nearly a duplicate of `idle`.** It exercises almost the same
  cop code path, differing only in the deadline value. The apply-default branch
  it was meant to cover requires `read.enabled` / `write.enabled`, which the
  mocks never set. Do not read a `keepalive` vs `idle` difference as meaningful;
  the observed differences are within the noise floor.
- **Active timeouts are never exercised.** No scenario sets one, so the entire
  active-timeout path is unmeasured, and a regression there would be invisible
  here.
- **The mock heap arena is denser than production's.** Mocks are allocated in a
  tight run, so the sweep gets better locality than real `UnixNetVConnection`s
  scattered across a long-running allocator. The baseline therefore, if
  anything, **slightly understates real cost** — and understates the large-N
  knee in particular.
- **The mock footprint is frozen at 1584 bytes** (`MOCK_FOOTPRINT_BYTES`,
  `sizeof(UnixNetVConnection)` as of commit `300f40c9`). It will not track
  `sizeof(UnixNetVConnection)` as the refactor changes that type. It is already
  24 bytes off in the optimized build (live size 1560). Keeping it frozen is
  deliberate — it holds the working set constant so checkpoints stay comparable
  — but it means the benchmark measures a fixed-size object, not the current
  one. Any deliberate rebaseline must be called out in the checkpoint entry.
- **No LTO, and no sanitizers, in either build** (see Environment). Production
  release builds may differ.
- **Single-threaded.** Real cops run concurrently on every network thread;
  `lock_contention` simulates failed acquisitions but not true cross-thread
  cache-line traffic.

---

# Checkpoint 1 — after Phase 1 (deadline check before lock, eager default timeout)

_Not yet run._

Expected: `get_mutex/run` falls well below N in scenarios where few connections
are due (`idle`, `churn`, `lock_contention`); `get_thread/run` still equals N,
because the `open_list` walk is untouched at this stage.

| | |
| --- | --- |
| commit | |
| build type | |
| date | |

```
(paste benchmark output here)
```

Notes:

---

# Checkpoint 2 — after the timer wheel switchover

_Not yet run._

Expected: `get_thread/run` collapses toward zero; large-N wall time drops
sharply and the super-linear knee at N=100000 flattens.

| | |
| --- | --- |
| commit | |
| build type | |
| date | |

```
(paste benchmark output here)
```

Notes:

---

# Checkpoint 3 — after the dead sweep machinery is removed

_Not yet run._

Expected: counters unchanged from checkpoint 2; this is a cleanup, so any
movement in the counters means the removal was not inert.

| | |
| --- | --- |
| commit | |
| build type | |
| date | |

```
(paste benchmark output here)
```

Notes:

---

# Checkpoint 4 — final pre-PR run

_Not yet run._

| | |
| --- | --- |
| commit | |
| build type | |
| date | |

```
(paste benchmark output here)
```

Notes:
