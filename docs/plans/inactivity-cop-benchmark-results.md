# InactivityCop benchmark — measurement log

A running log of `InactivityCop` benchmark results across the timer-wheel
refactor, with the code change that produced each entry. Append one entry per
measured point; never edit an existing entry's numbers. The tables and reading
instructions must stay interpretable by someone who has never seen this work.

The plan this log tracks is
[`2026-09-17-inactivity-cop-timer-wheel.md`](2026-09-17-inactivity-cop-timer-wheel.md).

## Trajectory at a glance

`idle` at N=100,000 is the headline scenario: many connections, none of them due,
which is the case the refactor exists to fix. `get_mutex/run` and `get_thread/run`
are exact counters with zero variance and are the primary signal; wall time is
noisy and only comparable within a measurement session (see the rules below).

| # | Point | Commit | `idle@100k` | `get_mutex/run` | `get_thread/run` | Change that produced it |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Baseline (old base) | `e86884d264` | 13.80 ms | N | N | Unmodified cop. Reference only — measured on a base 63 upstream commits older than everything after it. |
| 2 | Pre-Phase-1 (re-measured) | `759cbac375` | 15.88 ms | N | N | Same code as #1, re-measured on the current base. **This is the true reference for #3.** |
| 3 | Checkpoint 1 — Phase 1 | `17cd32cc0f` | **9.07 ms** | **0** | N | Deadline pre-check before the lock; global default timeout applied at `startCop`. |
| 4 | Checkpoint 2 — timer wheel | `b6b604b8ec` | 0.0001 ms* | 0 | **0** | Wheel replaces the `open_list` refill walk. *See entry 4's measurement caveat: a benchmark call is no longer a tick. |
| 5 | Checkpoint 3 — sweep removed | _pending_ | | | | `cop_list` and the TS-4612 epoll hook deleted. |
| 6 | Checkpoint 4 — pre-PR | _pending_ | | | | Final run from a clean build. |

## Rules that make entries comparable

Learned the hard way; all three have already bitten this log once.

1. **Counters are truth, wall time is an estimate.** `get_mutex/run`,
   `get_thread/run`, `cb/run` and `lockfail/run` are exact and reproduce
   byte-identically across machines and builds. Quote those. Wall-clock
   percentages are supporting evidence.
2. **Wall time is only comparable within a single measurement session.** Between
   sessions on the same machine and the *same commit*, `idle@100k` has been
   observed at 7.20 ms and 9.07 ms — a 26% gap, larger than the 5-15%
   within-session noise floor. So: **when adding an entry, re-measure the
   previous point back-to-back in the same session** rather than comparing
   against a number recorded earlier in this file.
3. **Record the base, and re-measure if it moved.** Entry 1 and entry 3 are
   separated by 63 upstream commits because the branch was rebased mid-stream.
   Deltas computed across that are meaningless. Entry 2 exists solely to give
   entry 3 a same-base reference.

Also: `MOCK_FOOTPRINT_BYTES` is frozen at 1584 deliberately. Do **not** update it
to track `sizeof(UnixNetVConnection)` as the refactor changes that type — a
changing mock stride would silently change the cache profile between entries.
Confirm `sizeof(PaddedMock)` is still 1584 in each entry's `[sizes]` line.

## How to add an entry

```sh
# 1. Measure the previous point on the current base, in this session.
git checkout --quiet <previous-entry-commit>
cmake --build build-bench --target test_net
./build-bench/src/iocore/net/test_net '[inactivity_cop]'   # 3 times

# 2. Return and measure the new point.
git checkout --quiet <branch>
cmake --build build-bench --target test_net
./build-bench/src/iocore/net/test_net '[inactivity_cop]'   # 3 times
```

Then append a section with: commit, the commit it is compared against, build
config, **what changed in the code since the previous entry**, the counter table,
the wall-time table with deltas, and any scenario that stopped being comparable.
Update the trajectory table above. If a scenario's *setup* changed, say so loudly
— see the `lock_contention` rebaseline note below for the format.

---

# Entry 1 — Baseline (unmodified cop)

Measured immediately before any change, on base `a2011c2fc4`.
**Reference only: 63 upstream commits older than entries 2 onward.** Use entry 2
as the pre-refactor comparison point.

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

> **`lock_contention` was rebaselined at commit `8f93ff6715`.** It originally used idle connections, whose mutexes the cop happened to lock incidentally. Once the cop stopped locking connections with nothing to do, that scenario recorded zero lock failures and no longer exercised the path it is named for, so its setup was switched to already-expired connections (the same helper `mass_expiry` uses) — the cop must now lock every one of them to fire the timeout, and the exact-equality assertion on the held 10% still holds.
>
> **The `lock_contention` rows recorded below are therefore not comparable to any run after `8f93ff6715`.** The workload changed, not just the code: the old rows show `cb/run = 0`, the new scenario fires ~90% of N as callbacks. Do not read the drop from 14.39 ms to ~12.17 ms at N=100,000 as an improvement — it is a different measurement. Every other scenario remains directly comparable.


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

**Between-session variance is larger still, and this is the trap that matters.**
The numbers above are three runs back-to-back in one session. Across *different*
sessions on the same machine at the **same commit** (`17cd32cc0f`), `idle@100k`
was measured at 7.20 ms in one session and 9.07 ms in another — a 26% gap with
no code difference at all, well outside the within-session floor. Entry 1 vs
entry 2 shows the same effect on identical cop code (13.80 vs 15.88 ms).

Consequence: **never compare a fresh measurement against a number recorded
earlier in this file.** Re-measure the previous point back-to-back in the same
session. This is rule 2 at the top, and it is why entry 2 exists.

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

# Entry 2 — Pre-Phase-1, re-measured on the current base

**Changes since entry 1: none to the cop.** Same source, different base. The
branch was rebased onto `upstream/master`, pulling in 63 upstream commits
(380 files, +22602/−3524). Exactly one of them touches a timeout-relevant file
(`fb3bb9b1ca`, clang-tidy `misc-redundant-expression`), and its only effect on
`NetHandler.cc` is a comment plus a `// NOLINT` in `startIO` — not the cop path.

This entry exists so entry 3 has a same-base, same-session reference.

| | |
| --- | --- |
| commit | `759cbac375` (rebased equivalent of entry 1's code) |
| base | `40253538ba` |
| build | RelWithDebInfo (`-O3 -g -DNDEBUG`), build-bench |
| host | pebs.local (M4 Max), Apple clang 21.0.0 |
| date | 2026-09-17 |
| method | 3 runs, 25 samples each after 2 warm-up runs; mean of the per-run means |
| sizes | `sizeof(PaddedMock)=1584`, live `sizeof(UnixNetVConnection)=1560` — unchanged from entry 1 |

| scenario | N=1000 | N=10000 | N=100000 | `get_mutex/run` | `get_thread/run` |
| --- | --- | --- | --- | --- | --- |
| `idle` | 0.0133 | 0.1917 | 15.8835 | N | N |
| `keepalive` | 0.0132 | 0.1841 | 15.7403 | N | N |
| `churn` | 0.0135 | 0.1839 | 16.3052 | N | N |
| `mass_expiry` | 0.0141 | 0.1889 | 16.5002 | N | N |
| `lock_contention` | 0.0186 | 0.2289 | 16.4199 | N | N |

Note this reads ~15% *slower* at N=100000 than entry 1 on identical cop code
(15.88 vs 13.80 ms for `idle`). That gap is environmental, not a code change, and
is the clearest single illustration of why rule 2 above exists.

`lock_contention` here still uses the original idle-based setup; it was
rebaselined later at `8f93ff6715`, so this row is not comparable to entry 3's.

---

# Entry 3 — Checkpoint 1, after Phase 1

**Changes since entry 2**, both in `src/iocore/net/`:

| Commit | Change |
| --- | --- |
| `8f93ff6715` | `P_InactivityCop.h`: a pre-check before `MUTEX_TRY_LOCK` that `continue`s when the locked body provably has nothing to do. It is a proven superset of the body's conditions; the locked body is byte-identical. Also rebaselined the benchmark's `lock_contention` scenario (see below). |
| `17cd32cc0f` | `NetHandler.cc`: `startCop()` applies the global default inactivity timeout eagerly; the lazy `-1` fixup and its pre-check clause are deleted from the cop. Benchmark fixture switched to call the real `startCop()`. |

Supporting commits with no effect on measurements: `9345864863` (doc),
`1e4be9b765` / `f9d8672efb` (this log).

Expected: `get_mutex/run` falls well below N in scenarios where few connections
are due (`idle`, `keepalive`, `churn`); `get_thread/run` still equals N, because
the `open_list` walk is untouched at this stage. `mass_expiry` and
`lock_contention` should both stay at `get_mutex/run == N` by construction —
every connection in them is due, so every one of them must be locked.

**Outcome: as expected on every counter. See the base-change warning below — the
wall-time comparison is against a re-measured pre-Phase-1 point, not against the
original baseline table at the top of this file.**

| | |
| --- | --- |
| commit | `17cd32cc0f` |
| compared against | `759cbac375` (pre-Phase-1, **re-measured on the same base**) |
| build type | RelWithDebInfo (`-O3 -g -DNDEBUG`), build-bench |
| host | pebs.local (M4 Max), Apple clang 21.0.0 |
| date | 2026-09-17 |
| method | 3 independent runs per point, 25 samples each after 2 warm-up runs; figures are the mean of the 3 per-run means |

## Base change: why the original baseline table is not the comparison point

Between the baseline being recorded and Phase 1 being implemented, the branch was
**rebased onto `upstream/master`**, pulling in 63 upstream commits (380 files,
+22602/−3524). This was not intended and was not part of the plan.

The original baseline rows at the top of this file were measured on the old base,
so comparing them against post-rebase numbers mixes our change with 63 commits of
upstream drift. To get a clean delta, the pre-Phase-1 state was **re-measured on
the current base** at `759cbac375` (the rebased equivalent of the baseline
commit), and that is what the table below compares against.

Two checks bound how much the base change could have affected the measurement:

- Of the 63 upstream commits, exactly one touches any timeout-relevant file
  (`fb3bb9b1ca`, clang-tidy `misc-redundant-expression`), and its only effect on
  `NetHandler.cc` is a comment plus a `// NOLINT` annotation in `startIO` — not
  on the cop path.
- `sizeof(UnixNetVConnection)` is 1560 in both the baseline and current runs, and
  `sizeof(PaddedMock)` is 1584 in both, so the mock stride and cache profile are
  unchanged. This is exactly the invariant the frozen footprint exists to protect.

The old baseline rows remain useful as a record of the pre-refactor cost, but
**do not compute deltas across them.** Note the re-measured pre-Phase-1 numbers
at N=100000 are noticeably higher than the original baseline (15.9 ms vs 13.8 ms
for `idle`), which is itself a reminder of how much run-to-run and
environment-to-environment variation this benchmark carries at large N.

## Counter columns — the exact, hardware-independent signal

| scenario | `get_mutex/run` before | `get_mutex/run` after | `get_thread/run` |
| --- | --- | --- | --- |
| `idle` | N | **0** | N (unchanged) |
| `keepalive` | N | **0** | N (unchanged) |
| `churn` | N | **N/100** (the 1% actually due) | N (unchanged) |
| `mass_expiry` | N | N (by construction) | N (unchanged) |
| `lock_contention` | N | N (by construction) | N (unchanged) |

The cop no longer takes a single `ProxyMutex` for a connection that has nothing
to do. These counters are unaffected by the base change — they are structural.
`get_thread/run` is still exactly N in all fifteen rows, confirming the
`open_list` refill walk is untouched; that is Checkpoint 2's job.

## Wall time, mean_ms — both points on the same base

| scenario | N | pre-Phase-1 | Checkpoint 1 | change |
| --- | --- | --- | --- | --- |
| `idle` | 1000 | 0.0133 | 0.0096 | −28% |
| `idle` | 10000 | 0.1917 | 0.1696 | −12% |
| `idle` | 100000 | 15.8835 | 9.0687 | **−43%** |
| `keepalive` | 1000 | 0.0132 | 0.0094 | −29% |
| `keepalive` | 10000 | 0.1841 | 0.1726 | −6% |
| `keepalive` | 100000 | 15.7403 | 7.8456 | **−50%** |
| `churn` | 1000 | 0.0135 | 0.0102 | −24% |
| `churn` | 10000 | 0.1839 | 0.1742 | −5% |
| `churn` | 100000 | 16.3052 | 9.6964 | **−41%** |
| `mass_expiry` | 1000 | 0.0141 | 0.0153 | +9% |
| `mass_expiry` | 10000 | 0.1889 | 0.1991 | +5% |
| `mass_expiry` | 100000 | 16.5002 | 15.9492 | −3% |

`lock_contention` is excluded: its scenario was rebaselined at `8f93ff6715`, which
is after `759cbac375`, so the two points measure different workloads (idle-based
before, expiry-based after). Its Checkpoint 1 figure is 15.97 ms at N=100000.

## `mass_expiry`: the honest cost of the pre-check

When *every* connection is due, the pre-check saves nothing — it performs the
unlocked reads and then the locked body re-reads the same fields and does the work
anyway. That shows up as roughly +5 to +9% at N=1000 and N=10000, where the added
per-connection loads are a visible fraction of a very short run, and washes out to
−3% at N=100000 where memory stalls dominate.

This is the correct trade. The case this refactor targets is many idle
connections, where the saving is 43-50%; the all-expiring case is rare and is
already dominated by callback work.

## Correction to an earlier reading of this data

An earlier version of this section compared Checkpoint 1 against the original
(pre-rebase) baseline and reported `idle` at −34% and `mass_expiry` as a **+13%
regression**. Both were artifacts of comparing across the base change. On a
same-base comparison `idle` improves more than reported (−43%) and `mass_expiry`
is roughly flat rather than a significant regression.

## Notes

- No production behavior changed: the pre-check is a proven superset of the
  conditions the locked body acts on, and the locked body is byte-identical to
  its pre-Phase-1 form.
- The `AuTest` timeout suite (`tests/gold_tests/timeout/`) has **not** been run
  against these commits and remains the real regression gate, particularly
  `default_inactivity_timeout.test.py` for the eager-default change.

---

# Entry 4 — Checkpoint 2, after the timer wheel switchover

Expected: `get_thread/run` collapses toward zero; large-N wall time drops
sharply and the super-linear knee at N=100000 flattens.

**Outcome: `get_thread/run` went to exactly 0 and per-call cost collapsed. But
read the measurement caveat below before quoting the wall-clock figures — the
switchover changed what a benchmark "call" costs, so entry 3's numbers and these
are not measuring the same thing.**

**Changes since entry 3:**

| Commit | Change |
| --- | --- |
| `19c2247824` | `NetEvent` gains `TimerWheelHook` + `LINK(NetEvent, timer_link)`; `NetHandler` gains a `TimerWheel<NetEvent>`, `rearm_timer()`, `_earliest_deadline()`; every deadline write routed through `rearm_timer()` except `netActivity()` |
| `3967b2fe55` | `startCop` schedules, `stopCop` cancels; closed NetEvents re-armed to the next tick; `clear()` asserts not-scheduled |
| `b6b604b8ec` | **The switchover.** `check_inactivity()` drives `timer_wheel.expire()` via a `Fire` functor instead of draining `cop_list`. Default inactivity timeout now armed in `set_enabled`/`netActivity`/`cancel_inactivity_timeout` |

| | |
| --- | --- |
| commit | `b6b604b8ec` |
| build type | RelWithDebInfo (`-O3 -g -DNDEBUG`), build-bench |
| host | pebs.local (M4 Max), Apple clang 21.0.0 |
| date | 2026-09-22 |
| sizes | `sizeof(PaddedMock)` = 1584 (unchanged); live `sizeof(UnixNetVConnection)` = 1600, up from 1560 — the hook and link. Frozen footprint left alone, drift NOTE prints as designed |

## Counter columns — the structural result

| scenario | `get_thread/run` entry 3 | `get_thread/run` now |
| --- | --- | --- |
| every scenario, every N | N | **0** |

`check_inactivity()` no longer walks `open_list` at all. This is the change the
whole refactor exists for, and it is exact and variance-free. (`cop_list` and the
walk code still exist in the tree; entry 5 deletes them.)

> **`get_thread/run` has changed meaning at this entry — do not keep reading it as
> "refill-walk touches."** The walk it used to count is gone. The counter is
> instrumented on `MockNetEvent::get_thread()`, and the remaining caller is the
> `ink_assert(ne->get_thread() == this_ethread())` inside
> `NetHandler::rearm_timer()`. So it now counts **re-arms**, not sweep touches.
>
> It reads 0 in the three runs recorded below because their locks all succeeded.
> A run where the `lock_contention` mutexes actually caused failures showed
> `get_thread/run = 1696` — matching its `lockfail/run` exactly, because the
> lock-failure path re-arms each element it could not lock. Both readings are
> correct; they measure different things than the column did before.
>
> Practical consequence for entries 5 and 6: a **rise** in `get_thread/run` is no
> longer evidence the sweep came back. Verify that claim by reading the code or
> by grepping for the walk, not from this counter.

## Measured

```
scenario                 N    mean_ms     min_ms     max_ms      ns/conn  get_mutex/run get_thread/run     cb/run lockfail/run
idle                  1000     0.0001     0.0000     0.0001         0.06              0              0          0            0
idle                 10000     0.0001     0.0000     0.0001         0.01              0              0          0            0
idle                100000     0.0001     0.0000     0.0002         0.00              0              0          0            0
keepalive             1000     0.0001     0.0000     0.0001         0.05              0              0          0            0
keepalive            10000     0.0001     0.0000     0.0003         0.01              0              0          0            0
keepalive           100000     0.0004     0.0000     0.0019         0.00              0              0          0            0
churn                 1000     0.0001     0.0000     0.0001         0.05              0              0          0            0
churn                10000     0.0001     0.0000     0.0001         0.01              0              0          0            0
churn               100000     0.0001     0.0000     0.0002         0.00              0              0          0            0
mass_expiry           1000     0.0043     0.0000     0.1069         4.32              0              0          0            0
mass_expiry          10000     0.0557     0.0000     0.6162         5.57              0              0          0            0
mass_expiry         100000     0.4486     0.1798     0.7123         4.49           1696              0       1696            0
lock_contention       1000     0.0044     0.0000     0.1077         4.36              0              0          0            0
lock_contention      10000     0.0585     0.0000     0.6763         5.85              0              0          0            0
lock_contention     100000     0.3989     0.1649     0.5870         3.99           1696              0       1696            0
```

`idle` at N=100000: **15.88 ms (entry 2, pre-Phase-1) → 9.07 ms (entry 3) →
0.0001 ms**. Three consecutive runs pass all assertions, including an exact
equality that `mass_expiry` fires exactly N callbacks summed across samples.

`lock_contention` asserts only a lower bound (`total failures >= held count`).
An exact bound was attempted and is **not achievable** with this harness: it
advances through real time via sleeps, so whether any single call crosses a tick
boundary — and how many ticks it catches up on if it does — depends on OS
scheduling. A held mock was observed failing zero times in one call and twice in
another, and a first-sample exact check failed outright with `0 >= 1000`. Pinning
it down would need an injectable clock. The lower bound still catches the bug
that matters: a held element being lost from the wheel entirely.

## Measurement caveat: a benchmark call is no longer a tick

**Do not read "0.0001 ms" as "the wheel costs 0.0001 ms per second of
operation."** The switchover changed what the harness measures.

Under the old sweep, *every* `check_inactivity()` call did the full O(N) walk
regardless of wall-clock, so 25 rapid-fire calls each cost the same as a real
one-second tick. Under the wheel, a call only does work if it crosses a
one-second tick boundary. The harness fires 25 calls in well under a second, so
most of them advance no tick and correctly do nothing.

So these figures understate the wheel's true per-tick cost. The honest readings:

- `get_thread/run` = 0 is **exact and fully meaningful** — it is a structural
  property, not a timing artifact. Quote this.
- The wall-clock collapse is **directionally real but not a clean per-tick
  number.** A production cop is invoked exactly once per second, so every real
  call crosses a tick; the harness's do-nothing calls dilute the mean.
- `churn` shows `cb/run = 0` for the same reason: its 1%-per-tick expiry needs a
  tick to elapse, and the sampling loop is faster than that.

To get a true per-tick comparison the harness would need to force a tick advance
per call (e.g. an injectable clock). That is worth doing before quoting a
speedup multiplier in the PR. Until then, lead with the counter columns.

A second harness artifact worth knowing: every mock shares an identical deadline,
so they all land in **one** bucket. Production deadlines are staggered by arrival
time and spread across buckets naturally. That makes `mass_expiry` and
`lock_contention` here a worst-case single-bucket drain — which is why they are
budget-limited to `TIMEOUT_BUDGET` (4096) per call, visible as `cb/run = 1696`.

## Fixture bug found and fixed while taking this measurement

The fixture never called `nh.timer_wheel.init()` — production does it in
`initialize_thread_for_net()`, but the benchmark builds its own `NetHandler`. So
`_cursor` sat at 0 against a `now_tick` of ~1.8e9, and the first `expire()` hit
the cursor-lag clamp and walked 4096 ticks in a single call. That produced a
cascade of confusing artifacts (mass expiry spilling unpredictably, lock failures
exceeding the held count) which were initially worked around by loosening
assertions. With `init()` added, the exact assertions hold and the relaxations
were reverted. Worth remembering: **a wheel whose cursor is not initialized looks
like a wheel with subtly wrong semantics.**

## Notes

- The switchover fixed a latent gap not in the original plan: the *only* code
  that applied `default_inactivity_timeout_in` was the cop's lazy sweep block,
  gated on `read.enabled || write.enabled`. Under the wheel a connection with no
  explicit timeout has no deadline, so it is never scheduled, so the cop never
  visits it, so the default never applies — **it would never time out.** Arming
  moved to `set_enabled`, `netActivity` (as an extension, so the hot path still
  does no wheel work), and `cancel_inactivity_timeout`.
- The `AuTest` timeout suite has **still not been run** against any of this, and
  it is the real gate — especially `default_inactivity_timeout.test.py`, which
  covers exactly the gap above.

---

# Entry 5 — Checkpoint 3, after the dead sweep machinery is removed

_Not yet run._

Expected: counters unchanged from entry 4; this is a cleanup, so any
movement in the counters means the removal was not inert.

**Changes since entry 4:** _(list the commits and what each did)_

| | |
| --- | --- |
| commit | |
| compared against | |
| build type | |
| date | |
| sizes | _(confirm `sizeof(PaddedMock)` is still 1584)_ |

```
(paste benchmark output here)
```

Notes:

---

# Entry 6 — Checkpoint 4, final pre-PR run

_Not yet run._

From a clean build. These are the numbers that go in the PR description.

**Changes since entry 5:** _(list the commits and what each did)_

| | |
| --- | --- |
| commit | |
| compared against | |
| build type | |
| date | |
| sizes | _(confirm `sizeof(PaddedMock)` is still 1584)_ |

```
(paste benchmark output here)
```

Notes:

---

# Appendix — TimerWheel bucket-count sizing sweep

A separate, one-off measurement of the wheel **primitive**, not of the cop. It
does not belong in the entry sequence above (different benchmark, different
units) but is recorded here so it is findable.

Landed in `36c3cf8b87`. Benchmark:
`./build-bench/src/tscore/test_tscore '[!benchmark][TimerWheel]'`.
100,000 continuously re-arming elements, 3600 simulated one-second ticks, seed
`0xBEEF`, build-bench (RelWithDebInfo). "Visits" are counted by instrumenting
`deadline_of()`, which the wheel calls exactly once per element popped — on a
genuine fire and on a clamp-driven lazy rearm alike. The counts are exact and
build-independent.

| buckets | timeout | total visits | visits/elem/period | mem/wheel | x32 threads |
| --- | --- | --- | --- | --- | --- |
| 256 | 30 s | 12,000,000 | 1.00 | 2 KiB | 64 KiB |
| 256 | 120 s | 3,000,000 | 1.00 | 2 KiB | 64 KiB |
| 256 | 4 h | 1,403,140 | 56.13 | 2 KiB | 64 KiB |
| 512 | 30 s | 12,000,000 | 1.00 | 4 KiB | 128 KiB |
| 512 | 120 s | 3,000,000 | 1.00 | 4 KiB | 128 KiB |
| 512 | 4 h | 701,306 | 28.05 | 4 KiB | 128 KiB |
| 1024 | 30 s | 12,000,000 | 1.00 | 8 KiB | 256 KiB |
| 1024 | 120 s | 3,000,000 | 1.00 | 8 KiB | 256 KiB |
| 1024 | 4 h | 314,857 | 12.59 | 8 KiB | 256 KiB |
| **4096** | 30 s | 12,000,000 | 1.00 | 32 KiB | 1 MiB |
| **4096** | 120 s | 3,000,000 | 1.00 | 32 KiB | 1 MiB |
| **4096** | 4 h | 25,183 | 1.01 | 32 KiB | 1 MiB |

**Conclusion: default raised from 1024 to 4096.** Every candidate size already
exceeds the 30 s and 120 s timeouts, so those rows are flat at exactly 1.00
visits per element per period — the dominant keepalive case is indifferent to
bucket count. The constant only matters for multi-hour timeouts (tunnel active
timeouts), where 1024 imposes real re-insertion churn for no offsetting benefit,
at a memory cost that is negligible either way.

**Caveat on the 4 h row.** A 1-hour observation window is shorter than a 4-hour
timeout, so for the larger rings most elements never complete a revisit cycle
and this table *understates* their steady-state churn. Longer windows
(14400/57600/144000 ticks, same seed) converge to ~14.9 visits/elem/period at
1024 and ~4.0 at 4096. The ranking and the decision are unaffected; the 1.01
figure in the 4096 row specifically should not be quoted as steady state.

The bucket count is now a template parameter, `TimerWheel<C, Buckets = 4096, L>`,
so re-running this sweep does not require editing the header.
