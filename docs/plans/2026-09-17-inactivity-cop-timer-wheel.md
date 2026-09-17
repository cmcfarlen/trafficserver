# InactivityCop Timer Wheel Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace `InactivityCop`'s unconditional O(N)-per-second scan of every open connection with a per-thread timer wheel whose cost is proportional to the number of connections actually reaching their deadline.

**Architecture:** Add a generic intrusive timer wheel (`include/tscore/TimerWheel.h`) — a ring of `DLL` buckets, one per second — and give each `NetEvent` a hook in it. Connections are inserted once at accept/connect and relinked only when their bucket comes due ("lazy rearm"), so the per-I/O activity path does no wheel work at all. `InactivityCop` survives as the *driver* (same `schedule_every` cadence, same debug tags, same queue trimming) but its body becomes `wheel.expire()` instead of a full-list sweep.

**Tech Stack:** C++20, CMake (`mydev` preset), Catch2 unit tests (`test_tscore`, `test_net`), AuTest end-to-end tests (`tests/gold_tests/timeout/`).

---

## Background: what the current code does

Read these before starting. All line numbers are as of the branch point.

- `src/iocore/net/UnixNet.cc:82-172` — `InactivityCop::check_inactivity`, the whole algorithm.
- `src/iocore/net/UnixNet.cc:187-193` — one cop per ET_NET thread, `schedule_every(HRTIME_SECONDS(proxy.config.net.inactivity_check_frequency))`, default 1s.
- `include/iocore/net/NetHandler.h:106-115` — `open_list` (every NetEvent on this thread) and `cop_list` (this second's work queue).
- `src/iocore/net/NetHandler.cc:100-119` — `startCop`/`stopCop` maintain `open_list`.
- `src/iocore/net/ReadWriteEventIO.cc:50-58` — the one existing optimization (TS-4612): epoll activity removes a NetEvent from `cop_list`, since it cannot have timed out this second.
- `include/iocore/net/NetEvent.h:88-125` — the timeout state: plain timestamps, no ordering.
- `src/iocore/net/UnixNetVConnection.cc:944` — `netActivity()`, the hot per-I/O deadline refresh.

Each run of the cop:

1. Drains `cop_list`. Per entry: `MUTEX_TRY_LOCK` on the **NetEvent's** mutex, free if closed, lazily apply the global default inactivity timeout if none was set, compare deadlines against `now`, and `ne->callback(VC_EVENT_*_TIMEOUT)` inline.
2. Refills `cop_list` by walking **all** of `open_list`.
3. `manage_active_queue(nullptr, true)` and `manage_keep_alive_queue()`.

### Why it is slow

Per thread, per second, with N open connections:

- **Step 2 is unconditionally O(N)** and never skippable. It pointer-chases `open_list` calling the *virtual* `get_thread()` on every element, touching several cache lines of a cold multi-hundred-byte `NetEvent` each time, in random order. A connection with a 30s keepalive timeout is visited 30 times before it can possibly fire.
- **Step 1 locks before it knows whether it needs to.** The `MUTEX_TRY_LOCK` happens *before* any deadline comparison — an atomic RMW on a cache line shared with the owning `HttpSM`, for every non-triggered connection, every second. Only the callback path needs that lock.
- **It runs with `NetHandler::mutex` held** on the poll thread, so the cost lands as a once-per-second latency spike in `waitForActivity`, not as smooth overhead. Timeout callbacks run inline, so a correlated wave of timeouts (origin outage, LB drain) stalls every other connection on the thread.
- **Cost scales with N, not with the number of expiring connections** — the wrong shape, since idle connections are exactly the ones that are cold in cache.

Today the only mitigation is `proxy.config.net.inactivity_check_frequency`, which trades timeout accuracy linearly for relief.

### Invariants any replacement must preserve

The cop quietly does four unrelated jobs. Breaking any of these is how this refactor goes wrong:

1. **Reaping `ne->closed` NetEvents it happens to encounter.** Several paths rely on the cop as a backstop to free: `UnixNetVConnection.cc:987`, `UnixNetVConnection.cc:1177`, `QUICNetVConnection.cc:358`, and the `EPOLLERR` reaping added in `8aabf92487`. Today a closed NetEvent is reaped within one second. **A naive wheel would let it linger until its deadline** — a slow fd leak. Task 9 handles this.
2. **Lazy application of the global default inactivity timeout**, including the `-1` vs `0` sentinel distinction that lets an override plugin mean "no timeout" (`NetEvent.h:98-107`). Task 3 moves this earlier, which is also a *prerequisite* for the wheel: the wheel needs a deadline at insert time.
3. **`keep_alive_queue` / `active_queue` trimming** and the `keep_alive_queue_timeout_*` / `default_inactivity_timeout_*` metrics.
4. **Second-granularity semantics** that `tests/gold_tests/timeout/` and the `inactivity_cop*` debug tags assume.

### The one correctness rule for the whole refactor

The wheel caches a deadline in each element's hook. That cached value **must never be later than the element's true deadline**, or a timeout fires late.

- **Extending** a timeout (the hot path — `netActivity()` on every read/write) needs **no wheel work at all**. The cached deadline is merely early; when the bucket comes due, `expire()` re-reads the real field, sees the future deadline, and relinks. That is lazy rearm, and it is why this design is cheap.
- **Setting or shortening** a timeout must re-arm the wheel. These are per-transaction, not per-byte, and the object is already hot in cache.

Task 8 enforces this by construction with a single `_rearm_timer()` helper called from every site that writes a deadline field, rather than by per-site reasoning.

### Considered and rejected

Do not implement these; they are recorded so the choice is not relitigated.

- **Min-heap / intrusive rbtree (nginx, Envoy style).** O(log N) insert with pointer chasing, on a workload where deadlines cluster hard on a handful of config values and resets dominate — a bucketed wheel handles that in O(1). Its one real advantage, deriving the epoll timeout from the earliest deadline, is a separate follow-up worth stealing later.
- **Dense SoA `{deadline, NetEvent*}` array scan.** Fixes the memory layout of the sweep (1.6 MB sequential and vectorizable at 100k conns, instead of 100k random misses) but stays O(N) per tick. It was the fallback if the wheel proved too invasive; the wheel is strictly better.
- **Sharding the sweep across K ticks.** A stopgap, strictly better than raising `inactivity_check_frequency`, but still O(N) per period.
- **A scheduled `Event` per connection.** Trades the sweep for allocator and event-heap churn on the hottest path.
- **Kernel-side timeouts** (`IORING_OP_LINK_TIMEOUT`). Real, but Linux-and-io_uring-only and only covers the subset where an inactivity timeout maps onto one pending read. A complement, not a replacement. `TCP_USER_TIMEOUT`/`SO_KEEPALIVE` cannot express these semantics.

---

## Phase 0: Build and baseline

### Task 1: Get a working build in this worktree

**Gotcha:** `CMakeUserPresets.json` is gitignored, so the `mydev` preset **does not exist in this worktree**. Copy it in first.

**Files:**
- Copy: `../../../CMakeUserPresets.json` → `CMakeUserPresets.json` (worktree root)

**Step 1: Copy the user presets**

```bash
cp /Users/cmcfarlen/projects/oss/trafficserver/CMakeUserPresets.json .
```

**Step 2: Configure**

```bash
cmake --preset mydev
```

Expected: configures into `build-mydev/` (the `mydev` preset inherits `dev`, whose `binaryDir` is `${sourceDir}/build-${presetName}`). This is a fresh configure and takes several minutes.

**Step 3: Build and run the two test targets we will use**

```bash
cmake --build build-mydev --target test_tscore test_net
ctest --test-dir build-mydev -R "test_tscore|test_net"
```

Expected: PASS. **Record the baseline.** On macOS there are 5 known pre-existing ctest failures elsewhere in the suite (`ts::Random` dlopen and jsonrpcserver socket); `test_tscore` and `test_net` are not among them. If either of these two targets fails here, stop — that is a pre-existing problem, not something this plan caused.

**Step 4: Commit the plan**

```bash
git add docs/plans/2026-09-17-inactivity-cop-timer-wheel.md
git commit -m "Add plan for InactivityCop timer wheel refactor"
```

Do **not** commit `CMakeUserPresets.json` — it is gitignored for a reason.

---

## Phase 1: Cheap wins in the existing cop

Both tasks are independently shippable, land before any new data structure, and survive into the wheel design. Phase 1 alone is a meaningful improvement.

### Task 2: Check the deadline before taking the NetEvent lock

Removes ~N atomic RMWs per second per thread. Safe because the whole scheme is already ±1s accurate: losing the race costs at most one extra tick.

**Files:**
- Modify: `src/iocore/net/UnixNet.cc:96-152`

**Step 1: Restructure the loop body**

The current loop takes the lock first thing. Reorder so the lock is taken only when there is something to do. Replace the body from `MUTEX_TRY_LOCK` through the `closed` check with a cheap pre-check:

```cpp
    while (NetEvent *ne = nh.cop_list.pop()) {
      // Nothing below needs the NetEvent mutex unless this connection is
      // closed, is missing a default timeout, or has actually expired. The
      // relaxed reads race with the owning thread extending a deadline, which
      // costs at most one extra tick - the scheme is already +/-1s accurate.
      bool const needs_default = ne->default_inactivity_timeout_in.load(std::memory_order_relaxed) == -1 ||
                                 (ne->next_inactivity_timeout_at == 0 && (ne->read.enabled || ne->write.enabled));
      bool const expired = (ne->next_inactivity_timeout_at && ne->next_inactivity_timeout_at < now) ||
                           (ne->next_activity_timeout_at && ne->next_activity_timeout_at < now);

      if (!ne->closed && !needs_default && !expired) {
        continue;
      }

      // If we cannot get the lock don't stop just keep cleaning
      MUTEX_TRY_LOCK(lock, ne->get_mutex(), this_ethread());
      if (!lock.is_locked()) {
        Metrics::Counter::increment(net_rsb.inactivity_cop_lock_acquire_failure);
        continue;
      }

      if (ne->closed) {
        nh.free_netevent(ne);
        continue;
      }
```

Leave everything from the `default_inactivity_timeout_in == -1` check onward exactly as it is — it now re-reads the fields under the lock, which is the correct double-check.

**Step 2: Build**

```bash
cmake --build build-mydev --target traffic_server
```

Expected: builds clean.

**Step 3: Run the timeout autests**

Autests cannot be run by Claude directly (sandbox). Ask the user to run:

```bash
cd /Users/cmcfarlen/projects/oss/trafficserver/.claude/worktrees/inactivity-cop-refactor/build-mydev/tests
./autest.sh --sandbox /tmp/sb-cop --clean=none -f default_inactivity_timeout inactive_timeout active_timeout inactive_client_timeout
```

Expected: PASS. These are the tests that cover the code being changed.

**Step 4: Format and commit**

```bash
cmake --build build-mydev --target format
git add -u
git commit -m "Check timeout deadlines before locking in InactivityCop

The cop took a MUTEX_TRY_LOCK on every scanned NetEvent before it knew
whether anything needed doing, costing an atomic RMW per connection per
second on a cache line shared with the owning HttpSM. Only the callback,
free, and default-timeout paths need the lock."
```

### Task 3: Apply the default inactivity timeout eagerly

Prerequisite for the wheel — it needs a deadline at insert time — and it removes a reason for the cop to touch every connection at least once.

**Files:**
- Modify: `src/iocore/net/NetHandler.cc:100-108` (`startCop`)
- Modify: `src/iocore/net/UnixNet.cc:109-116` (drop the lazy `-1` fixup)

**Step 1: Apply the global default in `startCop`**

`startCop` is called once per connection at accept/connect, on the connection's own thread, with the NetHandler mutex held. Add before `open_list.enqueue(ne)`:

```cpp
  // The cop used to fill this in lazily, which forced it to visit every
  // connection at least once. -1 means no override plugin has set a
  // context-specific default, so the global applies.
  if (ne->default_inactivity_timeout_in.load(std::memory_order_relaxed) == -1) {
    ne->set_default_inactivity_timeout(HRTIME_SECONDS(config.default_inactivity_timeout));
  }
```

**Step 2: Delete the lazy fixup from the cop**

Remove the `if (ne->default_inactivity_timeout_in == -1) { ... }` block at `UnixNet.cc:109-116` and drop `needs_default`'s first clause from Task 2, leaving:

```cpp
      bool const needs_default = ne->next_inactivity_timeout_at == 0 && (ne->read.enabled || ne->write.enabled);
```

**Step 3: Verify the override-plugin ordering still works**

A plugin that sets a per-context default later (`TSVConnSetDefaultInactivityTimeout` and friends) still wins, because `set_default_inactivity_timeout` simply overwrites. Confirm by reading how `default_inactivity_timeout` is applied as an overridable config:

```bash
grep -rn "set_default_inactivity_timeout\|default_inactivity_timeout" src/api src/proxy/http --include='*.cc' | head -20
```

Expected: the plugin path calls `set_default_inactivity_timeout`, which is a plain store — unaffected by the sentinel no longer being `-1` after accept.

**Step 4: Run the autests**

Same command as Task 2 Step 3. `default_inactivity_timeout.test.py` is the direct gate here — it sets `proxy.config.net.default_inactivity_timeout: 2` both globally and per-remap (`tests/gold_tests/timeout/default_inactivity_timeout.test.py:72-75`), so it exercises both the global and the override path.

Expected: PASS.

**Step 5: Format and commit**

```bash
cmake --build build-mydev --target format
git add -u
git commit -m "Apply the global default inactivity timeout at startCop

Doing this lazily in the cop forced it to visit every open connection at
least once per second regardless of deadlines, and leaves the connection
with no usable deadline at insert time."
```

---

## Phase 2: The TimerWheel data structure (TDD)

`NetHandler` needs the whole event system to instantiate, so it is effectively untestable in isolation — `src/iocore/net/unit_tests/test_NetHandler.cc` only tests the pure `Config` struct. Therefore the wheel is built as a **standalone, dependency-free intrusive container in tscore**, where it can be driven hard by a fast unit test with a fake element type. This is both better design and the only way to TDD this work.

It is a template because it must work on a fake element in tests and on `NetEvent` in production; this matches the existing `List.h` idiom (`DList(_c,_f)` is already a template macro) and satisfies the CLAUDE.md "templates only when needed" bar.

### Design

A ring of `N_BUCKETS` intrusive `DLL` buckets, one per `TICK` (1 second). `_cursor` is the tick index of the last **fully drained** bucket.

Contract on the element type `C`, duck-typed the same way `List.h` already duck-types `C::Link_<field>`:
- `LINK(C, timer_link)` — the list linkage.
- a member `TimerWheelHook timer_hook` — the cached deadline plus current slot.

Note that `LINK(_c, _f)` (`include/tscore/List.h:105-130`) expands to *both* the traits class `Link_timer_link` *and* the storage member `Link<_c> timer_link` — the macro ends in `Link<_c> _f`, which is why the trailing semicolon matters. Do not declare the member separately. This is also what makes `typename C::Link_timer_link` a valid default for the wheel's `L` parameter.

Slot arithmetic, and the two aliasing traps that will otherwise cause an infinite loop:

- While draining the bucket at tick `T`, a re-inserted element must land in a bucket **other than `T`**. So all scheduling takes a `floor_tick` and clamps the result into `[floor_tick, floor_tick + N_BUCKETS - 2]`. During a drain of `T` the caller passes `floor_tick = T + 1`, giving a max tick of `T + N_BUCKETS - 1` — and never `T + N_BUCKETS`, which would alias back onto `T`.
- Hence the usable range is `N_BUCKETS - 1` ticks, not `N_BUCKETS`.

With `N_BUCKETS = 1024` the range is ~17 minutes at 8 KB per NetHandler. Deadlines beyond it (long tunnel active timeouts) are clamped and re-inserted on arrival: a 4-hour timeout is touched ~14 times total instead of 14,400.

### Task 4: Write the failing test for schedule/expire

**Files:**
- Create: `src/tscore/unit_tests/test_TimerWheel.cc`
- Modify: `src/tscore/CMakeLists.txt:141-172` (add to the `test_tscore` source list, alphabetically near `unit_tests/test_Throttler.cc`)

**Step 1: Write the test file**

Include the Apache license header (copy the 20-line header from `src/tscore/unit_tests/test_List.cc:1-22`), then:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "tscore/TimerWheel.h"
#include "tscore/ink_hrtime.h"

namespace
{

// Minimal element satisfying the TimerWheel contract.
struct Conn {
  int            id       = 0;
  ink_hrtime     deadline = 0; // the "true" deadline the wheel re-reads
  TimerWheelHook timer_hook;

  LINK(Conn, timer_link);

  Conn(int i, ink_hrtime d) : id(i), deadline(d) {}
};

using Wheel = TimerWheel<Conn>;

// Fire callback that records ids, and reports each element's true deadline.
struct Recorder {
  std::vector<int> fired;

  ink_hrtime
  deadline_of(Conn *c) const
  {
    return c->deadline;
  }

  void
  operator()(Conn *c)
  {
    fired.push_back(c->id);
  }
};

} // namespace

TEST_CASE("TimerWheel fires an element once its deadline passes", "[libts][TimerWheel]")
{
  ink_hrtime const t0 = HRTIME_SECONDS(1000);
  Wheel            w;
  w.init(t0);

  Conn c{1, t0 + HRTIME_SECONDS(5)};
  w.schedule(&c, c.deadline);

  Recorder rec;

  // Not yet due.
  CHECK(w.expire(t0 + HRTIME_SECONDS(4), 1000, rec) == 0);
  CHECK(rec.fired.empty());

  // Due.
  CHECK(w.expire(t0 + HRTIME_SECONDS(6), 1000, rec) == 1);
  REQUIRE(rec.fired.size() == 1);
  CHECK(rec.fired[0] == 1);

  // Fired elements leave the wheel; they do not fire twice.
  CHECK(w.expire(t0 + HRTIME_SECONDS(60), 1000, rec) == 0);
  CHECK(rec.fired.size() == 1);
}
```

**Step 2: Add to CMake**

Insert `unit_tests/test_TimerWheel.cc` into the `test_tscore` source list in `src/tscore/CMakeLists.txt`.

**Step 3: Run it to verify it fails**

```bash
cmake --build build-mydev --target test_tscore
```

Expected: FAIL to compile — `tscore/TimerWheel.h` does not exist.

### Task 5: Implement the minimal TimerWheel

**Files:**
- Create: `include/tscore/TimerWheel.h`

**Step 1: Write the header**

Apache license header, then:

```cpp
#pragma once

#include <cstdint>
#include <utility>

#include "tscore/List.h"
#include "tscore/ink_hrtime.h"

/** Per-element state the TimerWheel manages.
 *
 * @a deadline is the wheel's cached copy of the element's deadline. It may be
 * earlier than the element's true deadline - extending a timeout deliberately
 * does no wheel work - but it must never be later, or the timeout fires late.
 * @a slot is the bucket the element currently sits in, or -1 if unscheduled.
 */
struct TimerWheelHook {
  ink_hrtime deadline = 0;
  int32_t    slot     = -1;
};

/** A hashed timer wheel over an intrusive list.
 *
 * @a C must provide `LINK(C, timer_link)` and a `TimerWheelHook timer_hook`
 * member. Cost per tick is proportional to the population of the buckets that
 * came due, not to the number of scheduled elements.
 *
 * Not thread safe; each instance is owned by one thread.
 */
template <class C, class L = typename C::Link_timer_link> class TimerWheel
{
public:
  static constexpr int32_t    N_BUCKETS = 1024;
  static constexpr ink_hrtime TICK      = HRTIME_SECOND;
  /// Longest deadline the wheel can hold directly; beyond this, elements are
  /// clamped and re-inserted when their bucket comes due.
  static constexpr int32_t MAX_TICKS_AHEAD = N_BUCKETS - 1;

  void
  init(ink_hrtime now)
  {
    _cursor = now / TICK;
  }

  /// Schedule (or reschedule) @a e for @a deadline.
  void
  schedule(C *e, ink_hrtime deadline)
  {
    cancel(e);
    e->timer_hook.deadline = deadline;
    _insert(e, _cursor + 1);
  }

  void
  cancel(C *e)
  {
    int32_t const slot = e->timer_hook.slot;

    if (slot >= 0) {
      _buckets[slot].remove(e);
      e->timer_hook.slot = -1;
    }
  }

  bool
  is_scheduled(C const *e) const
  {
    return e->timer_hook.slot >= 0;
  }

  /** Advance to @a now, firing elements whose deadline has arrived.
   *
   * @a f must provide `ink_hrtime deadline_of(C *)`, returning the element's
   * true deadline, and `void operator()(C *)` to fire it. An element whose true
   * deadline is still in the future is relinked rather than fired (lazy rearm),
   * which is what keeps the activity path free of wheel work. A deadline of 0
   * means "no timeout" and drops the element out of the wheel.
   *
   * Stops after @a budget elements have been fired, leaving the rest in place
   * for the next call, so a correlated wave of timeouts cannot monopolize the
   * thread.
   *
   * @return the number of elements fired.
   */
  template <typename F>
  int
  expire(ink_hrtime now, int budget, F &&f)
  {
    int64_t const now_tick = now / TICK;
    int           fired    = 0;

    while (_cursor < now_tick) {
      int64_t const   tick   = _cursor + 1;
      DLL<C, L>      &bucket = _buckets[tick & (N_BUCKETS - 1)];

      while (C *e = bucket.pop()) {
        e->timer_hook.slot = -1;

        ink_hrtime const deadline = f.deadline_of(e);
        if (deadline == 0) {
          continue;
        }
        if (deadline > now) {
          e->timer_hook.deadline = deadline;
          // floor is tick + 1: re-inserting into `tick` would loop forever.
          _insert(e, tick + 1);
          continue;
        }

        f(e);
        ++fired;
        if (fired >= budget) {
          // The bucket keeps whatever is left; _cursor is not advanced, so the
          // next call resumes here.
          return fired;
        }
      }
      _cursor = tick;
    }

    return fired;
  }

private:
  void
  _insert(C *e, int64_t floor_tick)
  {
    int64_t tick = e->timer_hook.deadline / TICK;

    if (tick < floor_tick) {
      tick = floor_tick;
    } else if (tick > floor_tick + MAX_TICKS_AHEAD - 1) {
      tick = floor_tick + MAX_TICKS_AHEAD - 1;
    }
    int32_t const slot = static_cast<int32_t>(tick & (N_BUCKETS - 1));

    e->timer_hook.slot = slot;
    _buckets[slot].push(e);
  }

  DLL<C, L> _buckets[N_BUCKETS];
  int64_t   _cursor = 0;
};
```

**Step 2: Run the test to verify it passes**

```bash
cmake --build build-mydev --target test_tscore
ctest --test-dir build-mydev -R test_tscore
```

Expected: PASS.

**Step 3: Commit**

```bash
cmake --build build-mydev --target format
git add include/tscore/TimerWheel.h src/tscore/unit_tests/test_TimerWheel.cc src/tscore/CMakeLists.txt
git commit -m "Add an intrusive timer wheel

A ring of intrusive lists bucketed by second, so expiry costs what the
due buckets hold rather than what the whole set holds. Lives in tscore
with no event system dependency so it can be tested directly."
```

### Task 6: Test lazy rearm, cancel, and the wheel-range wraparound

These are the cases that break a hand-rolled wheel. Each is one test; run and commit after each if you prefer smaller steps.

**Files:**
- Modify: `src/tscore/unit_tests/test_TimerWheel.cc`

**Step 1: Write the tests**

```cpp
TEST_CASE("TimerWheel relinks an element whose deadline moved out", "[libts][TimerWheel]")
{
  ink_hrtime const t0 = HRTIME_SECONDS(1000);
  Wheel            w;
  w.init(t0);

  Conn c{1, t0 + HRTIME_SECONDS(5)};
  w.schedule(&c, c.deadline);

  // Activity extends the true deadline without touching the wheel - this is
  // the whole point of the design.
  c.deadline = t0 + HRTIME_SECONDS(20);

  Recorder rec;
  CHECK(w.expire(t0 + HRTIME_SECONDS(6), 1000, rec) == 0);
  CHECK(rec.fired.empty());
  CHECK(w.is_scheduled(&c));

  CHECK(w.expire(t0 + HRTIME_SECONDS(21), 1000, rec) == 1);
  CHECK(rec.fired.size() == 1);
}

TEST_CASE("TimerWheel drops an element whose timeout was disabled", "[libts][TimerWheel]")
{
  ink_hrtime const t0 = HRTIME_SECONDS(1000);
  Wheel            w;
  w.init(t0);

  Conn c{1, t0 + HRTIME_SECONDS(5)};
  w.schedule(&c, c.deadline);
  c.deadline = 0; // no timeout

  Recorder rec;
  CHECK(w.expire(t0 + HRTIME_SECONDS(6), 1000, rec) == 0);
  CHECK_FALSE(w.is_scheduled(&c));
}

TEST_CASE("TimerWheel cancel removes an element", "[libts][TimerWheel]")
{
  ink_hrtime const t0 = HRTIME_SECONDS(1000);
  Wheel            w;
  w.init(t0);

  Conn c{1, t0 + HRTIME_SECONDS(5)};
  w.schedule(&c, c.deadline);
  CHECK(w.is_scheduled(&c));
  w.cancel(&c);
  CHECK_FALSE(w.is_scheduled(&c));

  Recorder rec;
  CHECK(w.expire(t0 + HRTIME_SECONDS(60), 1000, rec) == 0);
}

// A deadline past the wheel's range is clamped and must be re-inserted on
// arrival rather than firing early or aliasing onto the bucket being drained.
TEST_CASE("TimerWheel handles deadlines beyond its range", "[libts][TimerWheel]")
{
  ink_hrtime const t0  = HRTIME_SECONDS(1000);
  int const        far = Wheel::N_BUCKETS * 3;
  Wheel            w;
  w.init(t0);

  Conn c{1, t0 + HRTIME_SECONDS(far)};
  w.schedule(&c, c.deadline);

  Recorder rec;
  // Walk right up to the deadline one range at a time; it must never fire early
  // and must always still be scheduled.
  for (int s = 1; s < far; s += Wheel::N_BUCKETS / 2) {
    CHECK(w.expire(t0 + HRTIME_SECONDS(s), 1000, rec) == 0);
    CHECK(w.is_scheduled(&c));
  }
  CHECK(rec.fired.empty());

  CHECK(w.expire(t0 + HRTIME_SECONDS(far + 1), 1000, rec) == 1);
  CHECK(rec.fired.size() == 1);
}

TEST_CASE("TimerWheel budget defers the remainder to the next call", "[libts][TimerWheel]")
{
  ink_hrtime const t0 = HRTIME_SECONDS(1000);
  Wheel            w;
  w.init(t0);

  std::vector<Conn> conns;
  conns.reserve(10);
  for (int i = 0; i < 10; ++i) {
    conns.emplace_back(i, t0 + HRTIME_SECONDS(5));
  }
  for (auto &c : conns) {
    w.schedule(&c, c.deadline);
  }

  Recorder rec;
  ink_hrtime const now = t0 + HRTIME_SECONDS(6);

  CHECK(w.expire(now, 4, rec) == 4);
  CHECK(rec.fired.size() == 4);
  CHECK(w.expire(now, 4, rec) == 4);
  CHECK(rec.fired.size() == 8);
  CHECK(w.expire(now, 4, rec) == 2);
  CHECK(rec.fired.size() == 10);
  CHECK(w.expire(now, 4, rec) == 0);
}

// Many connections all sharing one timeout value is the real traffic pattern:
// deadlines cluster on a handful of config values.
TEST_CASE("TimerWheel fires a large clustered population exactly once each", "[libts][TimerWheel]")
{
  ink_hrtime const t0 = HRTIME_SECONDS(1000);
  int const        n  = 10000;
  Wheel            w;
  w.init(t0);

  std::vector<Conn> conns;
  conns.reserve(n);
  for (int i = 0; i < n; ++i) {
    conns.emplace_back(i, t0 + HRTIME_SECONDS(30 + (i % 4)));
  }
  for (auto &c : conns) {
    w.schedule(&c, c.deadline);
  }

  Recorder rec;
  for (int s = 1; s <= 40; ++s) {
    w.expire(t0 + HRTIME_SECONDS(s), 100000, rec);
  }

  REQUIRE(rec.fired.size() == static_cast<size_t>(n));
  std::vector<int> seen = rec.fired;
  std::sort(seen.begin(), seen.end());
  seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
  CHECK(seen.size() == static_cast<size_t>(n)); // no element fired twice
}
```

Add `#include <algorithm>` to the includes.

**Step 2: Run**

```bash
cmake --build build-mydev --target test_tscore
ctest --test-dir build-mydev -R test_tscore
```

Expected: PASS. If "deadlines beyond range" hangs, the clamp arithmetic in `_insert` is aliasing onto the bucket being drained — re-read the two traps in the Design section above.

**Step 3: Commit**

```bash
git add -u
git commit -m "Cover timer wheel rearm, cancel, wraparound, and budget"
```

### Task 7: Benchmark the wheel against the equivalent sweep

Establishes the claim quantitatively before touching the net code, at the data-structure level where it can be measured without a load generator.

**Files:**
- Create: `tools/benchmark/benchmark_TimerWheel.cc`
- Modify: `tools/benchmark/CMakeLists.txt`

**Step 1: Read an existing benchmark for the pattern**

```bash
sed -n '1,60p' tools/benchmark/benchmark_FreeList.cc
cat tools/benchmark/CMakeLists.txt
```

**Step 2: Write a benchmark** comparing, at N = 100,000 elements with a 30s timeout and 1s ticks over 60 simulated seconds:
- the wheel: total elements visited across all `expire()` calls;
- the current algorithm: a full N-element sweep per tick.

Report visited-element counts (the cache-miss proxy) and wall time for both. The expected shape is ~30x fewer visits for a 30s timeout, improving with longer timeouts.

**Step 3: Build and run**

```bash
cmake --preset mydev -B build-mydev -DENABLE_BENCHMARKS=ON
cmake --build build-mydev --target benchmark_TimerWheel
./build-mydev/tools/benchmark/benchmark_TimerWheel
```

Expected: wheel visits far fewer elements. **Record the numbers in the commit message** — this is the evidence for the whole refactor.

**Step 4: Commit**

```bash
cmake --build build-mydev --target format
git add tools/benchmark/benchmark_TimerWheel.cc tools/benchmark/CMakeLists.txt
git commit -m "Add a timer wheel vs full-sweep benchmark

<paste the measured numbers here>"
```

---

## Phase 3: Wire the wheel into NetHandler

`InactivityCop` stays as the driver — same cadence, same name, same `inactivity_cop*` debug tags, same queue trimming — so the diff is reviewable and the autests keep their meaning. Only its algorithm changes.

### Task 8: Give NetEvent a wheel hook and a single re-arm path

Enforces the correctness rule by construction rather than per-site reasoning.

**Files:**
- Modify: `include/iocore/net/NetEvent.h:88-140`
- Modify: `src/iocore/net/UnixNetVConnection.cc` (deadline write sites)
- Modify: `src/iocore/net/P_UnixNetVConnection.h:332`
- Modify: `src/iocore/net/P_UnixNet.h:234,259`

**Step 1: Add the hook to NetEvent**

In `include/iocore/net/NetEvent.h`, include `tscore/TimerWheel.h`, then alongside the existing `LINK` declarations:

```cpp
  LINK(NetEvent, timer_link);
  TimerWheelHook timer_hook;
```

**Step 2: Add the re-arm helper**

`NetEvent` can reach its `NetHandler` through `nh`. Add a public method:

```cpp
  /** Re-arm this NetEvent's slot in the NetHandler's timer wheel.
   *
   * Must be called after any change that makes a deadline *earlier* - setting
   * a timeout, shortening one, or closing. Extending a deadline needs no call:
   * the wheel re-reads the real deadline when the bucket comes due.
   */
  void rearm_timer();
```

Implement it in `src/iocore/net/NetEvent.cc` (create the file and add it to `src/iocore/net/CMakeLists.txt` if it does not exist; otherwise put it beside the existing NetEvent code), as the earlier of the two deadlines:

```cpp
void
NetEvent::rearm_timer()
{
  if (nh == nullptr) {
    return;
  }
  nh->rearm_timer(this);
}
```

with `NetHandler::rearm_timer(NetEvent *ne)` in `NetHandler.cc` computing the key and calling the wheel:

```cpp
void
NetHandler::rearm_timer(NetEvent *ne)
{
  ink_assert(ne->get_thread() == this_ethread());

  // A closed NetEvent must be reaped promptly rather than at its deadline, or
  // the fd lingers. Deadline 0 on both means no timeout at all.
  ink_hrtime deadline = 0;

  if (ne->closed) {
    deadline = ink_get_hrtime();
  } else {
    deadline = _earliest_deadline(ne);
  }

  if (deadline == 0) {
    timer_wheel.cancel(ne);
  } else {
    timer_wheel.schedule(ne, deadline);
  }
}
```

where `_earliest_deadline` is the non-zero minimum of `next_inactivity_timeout_at` and `next_activity_timeout_at`.

**Step 3: Call it from every site that writes a deadline**

Find them:

```bash
grep -rn "next_inactivity_timeout_at\s*=\|next_activity_timeout_at\s*=" src/iocore/net include/iocore/net
```

At the branch point that is: `UnixNetVConnection.cc:456` (`set_enabled`), `:944` (`netActivity`), `:1204` (`clear`), `:1296` (`set_inactivity_timeout`), `P_UnixNetVConnection.h:332`, `P_UnixNet.h:234,259` (read/write disable), plus `set_active_timeout` and the `*signal_timeout_at = 0` in `mainEvent`. Add `rearm_timer()` after each **except** `netActivity()` — that one only ever extends, and leaving it alone is the entire performance win. Put a comment there saying so:

```cpp
void
UnixNetVConnection::netActivity()
{
  Dbg(dbg_ctl_socket, "net_activity updating inactivity %" PRId64 ", NetVC=%p", this->inactivity_timeout_in, this);
  // Deliberately no rearm_timer(): this only ever pushes the deadline later,
  // and the wheel re-reads the real deadline when the bucket comes due. This
  // is the hot per-I/O path.
  if (this->inactivity_timeout_in) {
    this->next_inactivity_timeout_at = ink_get_hrtime() + this->inactivity_timeout_in;
  } else {
    this->next_inactivity_timeout_at = 0;
  }
}
```

`clear()` runs at free time; call `cancel()` there rather than `rearm_timer()`.

**Step 4: Build**

```bash
cmake --build build-mydev --target traffic_server
```

Expected: builds clean. Nothing uses the wheel to fire yet, so behavior is unchanged.

**Step 5: Commit**

```bash
cmake --build build-mydev --target format
git add -u
git commit -m "Add a timer wheel hook and single re-arm path to NetEvent

Routing every deadline write through rearm_timer() makes the wheel's
cached deadline never-later-than the true one by construction. netActivity
is the deliberate exception: it only extends, so lazy rearm covers it."
```

### Task 9: Schedule and cancel in startCop/stopCop

**Files:**
- Modify: `src/iocore/net/NetHandler.cc:100-119`
- Modify: `include/iocore/net/NetHandler.h` (add the `timer_wheel` member and `rearm_timer`/`_earliest_deadline` declarations)
- Modify: `src/iocore/net/UnixNet.cc:174-195` (`init` the wheel per thread)

**Step 1: Add the member**

In `NetHandler`, next to `cop_list`:

```cpp
  TimerWheel<NetEvent> timer_wheel;
```

**Step 2: Initialize it** in `initialize_thread_for_net`, after the NetHandler is constructed:

```cpp
  nh->timer_wheel.init(ink_get_hrtime());
```

**Step 3: Schedule in `startCop`, cancel in `stopCop`**

`startCop` — after the default timeout is applied (Task 3) and `open_list.enqueue(ne)`:

```cpp
  rearm_timer(ne);
```

`stopCop` — before `open_list.remove(ne)`:

```cpp
  timer_wheel.cancel(ne);
```

**Step 4: Make sure `closed` reaches the wheel**

Per invariant 1, a closed NetEvent must still be reaped within a tick. Find where `closed` is set and add a `rearm_timer()`:

```bash
grep -rn "closed = 1\|closed  = 1" src/iocore/net | head
```

Verify by reading `UnixNetVConnection.cc:987` and `:1177` (both comment "Send this netvc to InactivityCop") that those paths now land in the next bucket rather than at the original deadline.

**Step 5: Build and run the autests**

```bash
cmake --build build-mydev --target traffic_server
```

Ask the user to run the **whole** timeout suite now, since this is the first behavioral change:

```bash
cd /Users/cmcfarlen/projects/oss/trafficserver/.claude/worktrees/inactivity-cop-refactor/build-mydev/tests
./autest.sh --sandbox /tmp/sb-cop --clean=none -f timeout
```

Expected: PASS. The wheel is populated but the cop still drives expiry from `cop_list`, so behavior should be identical — any failure here is a bookkeeping bug in schedule/cancel, caught before the switchover.

**Step 6: Commit**

```bash
cmake --build build-mydev --target format
git add -u
git commit -m "Populate the timer wheel from startCop and stopCop

Nothing fires from the wheel yet; this lands the bookkeeping on its own
so a schedule/cancel bug is caught before the switchover."
```

### Task 10: Switch InactivityCop to the wheel

The actual switchover.

**Files:**
- Modify: `src/iocore/net/UnixNet.cc:82-172`

**Step 1: Replace the algorithm**

`check_inactivity` becomes: build a fire functor that carries the `NetHandler`, call `expire()`, then do the queue trimming exactly as before. The functor body is the *existing* per-NetEvent logic from the current loop — the lock, the closed check, the `keep_alive_queue` stat, the `ATS_PROBE6`, the `callback()` — lifted verbatim so the semantics and metrics do not drift:

```cpp
class InactivityCop : public Continuation
{
public:
  explicit InactivityCop(Ptr<ProxyMutex> &m) : Continuation(m.get()) { SET_HANDLER(&InactivityCop::check_inactivity); }

  /// Fire callback for the timer wheel. Reports each NetEvent's true deadline
  /// and applies the timeout, preserving the semantics of the old sweep.
  struct Fire {
    NetHandler &nh;
    ink_hrtime  now;
    Event      *e;

    ink_hrtime
    deadline_of(NetEvent *ne) const
    {
      if (ne->closed) {
        return now; // reap promptly
      }
      return nh._earliest_deadline(ne);
    }

    void operator()(NetEvent *ne);
  };

  int
  check_inactivity(int /* event */, Event *e)
  {
    ink_hrtime  now = ink_get_hrtime();
    NetHandler &nh  = *get_NetHandler(this_ethread());

    Dbg(dbg_ctl_inactivity_cop_check, "Checking inactivity on Thread-ID #%d", this_ethread()->id);

    Fire fire{nh, now, e};
    nh.timer_wheel.expire(now, TIMEOUT_BUDGET, fire);

    // Cleanup the active and keep-alive queues periodically
    nh.manage_active_queue(nullptr, true); // close any connections over the active timeout
    nh.manage_keep_alive_queue();

    return 0;
  }
};
```

`Fire::operator()` keeps the default-timeout arming block (`next_inactivity_timeout_at == 0 && default_inactivity_timeout_in > 0 && (read.enabled || write.enabled)`), the `MUTEX_TRY_LOCK` with the `inactivity_cop_lock_acquire_failure` metric, the `free_netevent` on closed, the `default_inactivity_timeout_count` and `keep_alive_queue_timeout_*` metrics, the `ATS_PROBE6`, and the inactivity-vs-active `callback()` choice — all unchanged from the current `UnixNet.cc:104-151`.

Pick `TIMEOUT_BUDGET` so a correlated wave cannot monopolize the thread; start generous (a few thousand) and note that anything deferred is picked up on the next tick.

**Note on the lock-failure path:** when the try-lock fails the old code just dropped the NetEvent, relying on the next second's refill to see it again. With the wheel there is no refill, so a lock failure **must** re-schedule: return the element to the wheel for the next tick instead of dropping it, or the connection never times out. This is the single most likely bug in this task.

**Step 2: Build**

```bash
cmake --build build-mydev --target traffic_server
```

**Step 3: Run the full timeout suite**

```bash
cd /Users/cmcfarlen/projects/oss/trafficserver/.claude/worktrees/inactivity-cop-refactor/build-mydev/tests
./autest.sh --sandbox /tmp/sb-cop --clean=none -f timeout
```

Expected: PASS — all of `default_inactivity_timeout`, `inactive_timeout`, `active_timeout`, `inactive_client_timeout`, `accept_timeout`, `conn_timeout`, `tunnel_active_timeout`, `http2_no_activity_timeout`, `http2_incomplete_header_timeout`.

**Step 4: Commit**

```bash
cmake --build build-mydev --target format
git add -u
git commit -m "Drive timeouts from the timer wheel instead of a full sweep

The cop keeps its cadence, debug tags, metrics, and queue trimming; only
the scan is replaced. Cost per tick is now proportional to the due buckets
rather than to every open connection on the thread."
```

### Task 11: Delete the dead sweep machinery

**Files:**
- Modify: `include/iocore/net/NetHandler.h:109` (remove `cop_list`)
- Modify: `src/iocore/net/NetHandler.cc:116` (`stopCop`)
- Modify: `src/iocore/net/ReadWriteEventIO.cc:50-58` (remove the `cop_list` hook)

**Step 1: Remove `cop_list` and its uses**

```bash
grep -rn "cop_list" src include | grep -v PreWarm
```

Every remaining reference should be removable: the declaration, the `stopCop` removal, and the TS-4612 hook in `ReadWriteEventIO::process_event`. That hook exists only to shrink the sweep; the wheel makes it pointless, and removing it also takes a branch and a write off the per-epoll-event path.

Keep `open_list` — it is still used for enumeration/shutdown. Verify:

```bash
grep -rn "open_list" src include
```

**Step 2: Build and run both unit test targets and the autests**

```bash
cmake --build build-mydev --target traffic_server test_net test_tscore
ctest --test-dir build-mydev -R "test_net|test_tscore"
```

Then the timeout suite again (same command as Task 10 Step 3).

Expected: PASS.

**Step 3: Commit**

```bash
cmake --build build-mydev --target format
git add -u
git commit -m "Remove cop_list now that the wheel orders timeouts

Also drops the TS-4612 epoll hook that existed only to shrink the sweep,
taking a branch and a store off the per-event path."
```

---

## Phase 4: Observability and docs

### Task 12: Make the cop's own cost visible

Right now the only telemetry on this code is `inactivity_cop_lock_acquire_failure` and an `ATS_PROBE6`, which is why the regression was invisible. Without this the improvement cannot be confirmed in production either.

**Files:**
- Modify: `src/iocore/net/Net.cc:114-116` (register metrics)
- Modify: `include/iocore/net/Net.h` or wherever `net_rsb` is declared
- Modify: `src/iocore/net/UnixNet.cc` (record them)
- Modify: `doc/admin-guide/monitoring/statistics/core/network-io.en.rst`

**Step 1: Add two metrics**

- `proxy.process.net.inactivity_cop_visited` — elements popped from buckets per run (the cost proxy: this is what used to be N per second).
- `proxy.process.net.inactivity_cop_deferred` — elements left unfired because the budget ran out, so budget starvation is visible rather than silent.

Follow the existing `Metrics::Counter::createPtr` pattern at `Net.cc:114`.

**Step 2: Document them** in `network-io.en.rst`, matching the surrounding `ts:stat` directive style.

**Step 3: Build, run unit tests, commit**

```bash
cmake --build build-mydev --target traffic_server
cmake --build build-mydev --target format
git add -u
git commit -m "Report inactivity cop visit and defer counts

The cop's own cost had no metric, which is why an O(N)-per-second scan
could regress silently at high connection counts."
```

### Task 13: Document the design

**Files:**
- Modify: `doc/developer-guide/` — find the right home:

```bash
grep -rln "InactivityCop\|inactivity" doc/developer-guide/ | head
```

**Step 1: Write a short section** covering: the wheel, the one correctness rule (extending is free, shortening must re-arm), why `netActivity()` deliberately does no wheel work, and the closed-NetEvent reaping requirement. Note that `proxy.config.net.inactivity_check_frequency` is now the wheel's tick rate, and that the wheel's range is `N_BUCKETS - 1` ticks with longer deadlines re-inserted on arrival.

**Step 2: Check the docs still build**

```bash
cmake --build build-mydev --target docs 2>&1 | tail -20
```

If there is no `docs` target in this configure, say so rather than claiming it passed.

**Step 3: Commit**

```bash
git add -u
git commit -m "Document the inactivity timeout wheel design"
```

---

## Validation before this becomes a PR

Unit tests and autests confirm *correctness*; they do not confirm the *performance* claim, which is the entire point. State plainly which of these were actually run.

1. **Unit tests:** `ctest --test-dir build-mydev -R "test_tscore|test_net"` — expect PASS. Full `ctest` has 5 known pre-existing macOS failures (`ts::Random` dlopen, jsonrpcserver socket); 163/168 is green on macOS.
2. **The whole timeout suite:** `-f timeout` — this is the real correctness gate.
3. **A broader autest run** for fallout beyond timeouts, since every connection now goes through `rearm_timer`: at minimum `-f keep_alive`, `-f h2`, `-f connect`.
4. **A high-connection-count load test** — the user's to run, since it needs their environment. The claim to check: `inactivity_cop_visited` per second should drop by roughly the ratio of the timeout duration to the tick (~30x for a 30s keepalive), and the once-per-second latency spike in the per-thread io stats should flatten. Compare against the same build with the wheel replaced by the old sweep, at the same connection count.
5. **Sanitizers.** The wheel is intrusive and hand-linked, so a bookkeeping bug is a use-after-free rather than a wrong answer. Run the timeout suite under the `myasan` preset before opening the PR:

```bash
cp /Users/cmcfarlen/projects/oss/trafficserver/CMakeUserPresets.json .
cmake --preset myasan
cmake --build build-mydev-asan --target traffic_server
```

## PR notes

- Branch is `worktree-inactivity-cop-refactor`; PR targets `master`.
- Phase 1 (Tasks 2-3) is independently valuable and could ship as its own PR if the wheel needs more review time.
- Label: this changes timeout dispatch timing at the margins. Not **Incompatible** — the observable semantics and all config keys are unchanged — but it deserves a careful reviewer and should not be backported to a release branch without load-test evidence.
- Reference TS-4612 (`425b696240`, `5b7aabccae`) in the PR description as the previous attempt at this problem, and explain that this replaces its `cop_list` mechanism rather than extending it.
