/** @file

  Basic locks for threads

  @par License

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

#include "tscore/ink_platform.h"
#include "tscore/Diags.h"
#include "iocore/eventsystem/Thread.h"

#ifdef ENABLE_LOCK_LEDGER
#include <atomic>
#endif

// Lock call sites carry their source location in debug builds and whenever the lock ledger is compiled in.
#if defined(DEBUG) || defined(ENABLE_LOCK_LEDGER)
#define TS_LOCK_SRCLOC 1
#else
#define TS_LOCK_SRCLOC 0
#endif

#define MAX_LOCK_TIME               HRTIME_MSECONDS(200)
#define THREAD_MUTEX_THREAD_HOLDING (-1024 * 1024)

/*------------------------------------------------------*\
|  Macros                                                |
\*------------------------------------------------------*/

/**
  Blocks until the lock to the ProxyMutex is acquired.

  This macro performs a blocking call until the lock to the ProxyMutex
  is acquired. This call allocates a special object that holds the
  lock to the ProxyMutex only for the scope of the function or
  region. It is a good practice to delimit such scope explicitly
  with '&#123;' and '&#125;'.

  @param _l Arbitrary name for the lock to use in this call
  @param _m A pointer to (or address of) a ProxyMutex object
  @param _t The current EThread executing your code.

*/

// A weak version of the SCOPED_MUTEX_LOCK macro, allows the mutex to be a nullptr.
#if TS_LOCK_SRCLOC
#define WEAK_SCOPED_MUTEX_LOCK(_l, _m, _t) WeakMutexLock _l(MakeSourceLocation(), (char *)nullptr, _m, _t);
#else // TS_LOCK_SRCLOC
#define WEAK_SCOPED_MUTEX_LOCK(_l, _m, _t) WeakMutexLock _l(_m, _t);
#endif // TS_LOCK_SRCLOC

#if TS_LOCK_SRCLOC
#define SCOPED_MUTEX_LOCK(_l, _m, _t) MutexLock _l(MakeSourceLocation(), (char *)nullptr, _m, _t)
#else // TS_LOCK_SRCLOC
#define SCOPED_MUTEX_LOCK(_l, _m, _t) MutexLock _l(_m, _t)
#endif // TS_LOCK_SRCLOC

/**
  Attempts to acquire the lock to the ProxyMutex.

  This macro attempts to acquire the lock to the specified ProxyMutex
  object in a non-blocking manner. After using the macro you can
  see if it was successful by comparing the lock variable with true
  or false (the variable name passed in the _l parameter).

  @param _l Arbitrary name for the lock to use in this call (lock variable)
  @param _m A pointer to (or address of) a ProxyMutex object
  @param _t The current EThread executing your code.

*/

#if TS_LOCK_SRCLOC
#define WEAK_MUTEX_TRY_LOCK(_l, _m, _t) WeakMutexTryLock _l(MakeSourceLocation(), (char *)nullptr, _m, _t);
#else // TS_LOCK_SRCLOC
#define WEAK_MUTEX_TRY_LOCK(_l, _m, _t) WeakMutexTryLock _l(_m, _t);
#endif // TS_LOCK_SRCLOC

#if TS_LOCK_SRCLOC
#define MUTEX_TRY_LOCK(_l, _m, _t) MutexTryLock _l(MakeSourceLocation(), (char *)nullptr, _m, _t)
#else // TS_LOCK_SRCLOC
#define MUTEX_TRY_LOCK(_l, _m, _t) MutexTryLock _l(_m, _t)
#endif // TS_LOCK_SRCLOC

/**
  Releases the lock on a ProxyMutex.

  This macro releases the lock on the ProxyMutex, provided it is
  currently held. The lock must have been successfully acquired
  with one of the MUTEX macros.

  @param _l Arbitrary name for the lock to use in this call (lock
    variable) It must be the same name as the one used to acquire the
    lock.

*/
#define MUTEX_RELEASE(_l) (_l).release()

/////////////////////////////////////
// DEPRECATED DEPRECATED DEPRECATED
#if TS_LOCK_SRCLOC
#define MUTEX_TAKE_TRY_LOCK(_m, _t) Mutex_trylock(MakeSourceLocation(), (char *)nullptr, _m, _t)
#else
#define MUTEX_TAKE_TRY_LOCK(_m, _t) Mutex_trylock(_m, _t)
#endif

#if TS_LOCK_SRCLOC
#define MUTEX_TAKE_LOCK(_m, _t) Mutex_lock(MakeSourceLocation(), (char *)nullptr, _m, _t)
#else
#define MUTEX_TAKE_LOCK(_m, _t) Mutex_lock(_m, _t)
#endif // TS_LOCK_SRCLOC

#define MUTEX_UNTAKE_LOCK(_m, _t) Mutex_unlock(_m, _t)
// DEPRECATED DEPRECATED DEPRECATED
/////////////////////////////////////

class EThread;
using EThreadPtr = EThread *;

#if TS_LOCK_SRCLOC
extern void lock_waiting(const SourceLocation &, const char *handler);
extern void lock_holding(const SourceLocation &, const char *handler);
extern void lock_taken(const SourceLocation &, const char *handler);
#endif

/**
  Lock object used in continuations and threads.

  The ProxyMutex class is the main synchronization object used
  throughout the Event System. It is a reference counted object
  that provides mutually exclusive access to a resource. Since the
  Event System is multithreaded by design, the ProxyMutex is required
  to protect data structures and state information that could
  otherwise be affected by the action of concurrent threads.

  A ProxyMutex object has an ink_mutex member (defined in ink_mutex.h)
  which is a wrapper around the platform dependent mutex type. This
  member allows the ProxyMutex to provide the functionality required
  by the users of the class without the burden of platform specific
  function calls.

  The ProxyMutex also has a reference to the current EThread holding
  the lock as a back pointer for verifying that it is released
  correctly.

  Acquiring/Releasing locks:

  Included with the ProxyMutex class, there are several macros that
  allow you to lock/unlock the underlying mutex object.

*/
class ProxyMutex : public RefCountObj
{
public:
  /**
    Underlying mutex object.

    The platform independent mutex for the ProxyMutex class. You
    must not modify or set it directly.

  */
  // coverity[uninit_member]
  ink_mutex the_mutex;

  /**
    Backpointer to owning thread.

    This is a pointer to the thread currently holding the mutex
    lock.  You must not modify or set this value directly.

  */
  EThreadPtr thread_holding;

  int nthread_holding;

#if TS_LOCK_SRCLOC
  ink_hrtime     hold_time;
  SourceLocation srcloc;
  const char    *handler;

#ifdef MAX_LOCK_TAKEN
  int taken;
#endif // MAX_LOCK_TAKEN

#ifdef LOCK_CONTENTION_PROFILING
  int  total_acquires, blocking_acquires, nonblocking_acquires, successful_nonblocking_acquires, unsuccessful_nonblocking_acquires;
  void print_lock_stats(int flag);
#endif // LOCK_CONTENTION_PROFILING
#endif // TS_LOCK_SRCLOC

#ifdef ENABLE_LOCK_LEDGER
  /// Label for this mutex in the lock ledger; non-null opts the mutex in to recording.
  const char *ledger_name = nullptr;
  /// Packed (thread id << 32 | call site id) of the current holder, 0 when free or not recorded.
  std::atomic<uint64_t> ledger_holder{0};
#endif

  void free() override;

  /**
    Constructor - use new_ProxyMutex() instead.

    The constructor of a ProxyMutex object. Initializes the state
    of the object but leaves the initialization of the mutex member
    until it is needed (through init()). Do not use this constructor,
    the preferred mechanism for creating a ProxyMutex is via the
    new_ProxyMutex function, which provides a faster allocation.

  */
  ProxyMutex()
#if TS_LOCK_SRCLOC
    : srcloc(nullptr, nullptr, 0)
#endif
  {
    thread_holding  = nullptr;
    nthread_holding = 0;
#if TS_LOCK_SRCLOC
    hold_time = 0;
    handler   = nullptr;
#ifdef MAX_LOCK_TAKEN
    taken = 0;
#endif // MAX_LOCK_TAKEN
#ifdef LOCK_CONTENTION_PROFILING
    total_acquires                    = 0;
    blocking_acquires                 = 0;
    nonblocking_acquires              = 0;
    successful_nonblocking_acquires   = 0;
    unsuccessful_nonblocking_acquires = 0;
#endif // LOCK_CONTENTION_PROFILING
#endif // TS_LOCK_SRCLOC
    // coverity[uninit_member]
  }

  /**
    Initializes the underlying mutex object.

    After constructing your ProxyMutex object, use this function
    to initialize the underlying mutex object.

  */
  void
  init()
  {
    ink_mutex_init(&the_mutex);
  }
};

// The ClassAllocator for ProxyMutexes
extern ClassAllocator<ProxyMutex, false> mutexAllocator;

#ifdef ENABLE_LOCK_LEDGER
/** Per-thread recording of ProxyMutex contention, written to binary files for offline analysis.

  Only mutexes with a @c ledger_name are recorded, unless TS_LOCK_LEDGER=all is set in the environment.
  See tools/lock_ledger/README.md for the file format and the analysis script.
 */
namespace lock_ledger
{
extern bool record_all;

/// Identifies the work being dispatched on this thread (continuation or net event), stamped on each entry.
extern thread_local uintptr_t current_context;

inline bool
enabled_for(const ProxyMutex *m)
{
  return m->ledger_name != nullptr || record_all;
}

/// Opt @a m in to recording under @a name, which must outlive the mutex.
void name(ProxyMutex *m, const char *name);
void on_miss(ProxyMutex *m, const SourceLocation &loc);
/// @a wait_start is when a blocking acquire began waiting, 0 if it did not wait; @a blocker is the holder seen then.
void on_acquired(ProxyMutex *m, const SourceLocation &loc, ink_hrtime wait_start, uint64_t blocker);
void on_release(ProxyMutex *m);

class ContextScope
{
public:
  explicit ContextScope(const void *ctx) : _saved(current_context) { current_context = reinterpret_cast<uintptr_t>(ctx); }
  ~ContextScope() { current_context = _saved; }

  ContextScope(const ContextScope &)            = delete;
  ContextScope &operator=(const ContextScope &) = delete;

private:
  uintptr_t _saved;
};
} // namespace lock_ledger

#define LOCK_LEDGER_NAME(_m, _name) lock_ledger::name(_m, _name)
#define LOCK_LEDGER_CONTEXT(_ctx)   lock_ledger::ContextScope _lock_ledger_ctx(_ctx)
#else
#define LOCK_LEDGER_NAME(_m, _name) ((void)0)
#define LOCK_LEDGER_CONTEXT(_ctx)   ((void)0)
#endif // ENABLE_LOCK_LEDGER

#if TS_LOCK_SRCLOC
/// Debug builds time every hold; a release build with the ledger only times the mutexes it records.
inline bool
lock_track_hold_time([[maybe_unused]] const ProxyMutex *m)
{
#ifdef DEBUG
  return true;
#else
  return lock_ledger::enabled_for(m);
#endif
}
#endif // TS_LOCK_SRCLOC

inline bool
Mutex_trylock(
#if TS_LOCK_SRCLOC
  const SourceLocation &location, const char *ahandler,
#endif
  ProxyMutex *m, EThread *t)
{
  ink_assert(t != nullptr);
  ink_assert(t == reinterpret_cast<EThread *>(this_thread()));
  if (m->thread_holding != t) {
    if (!ink_mutex_try_acquire(&m->the_mutex)) {
#if TS_LOCK_SRCLOC
      // Report the waiting site, not the holder's. m->srcloc and m->handler
      // belong to whichever thread holds the mutex, and this thread just failed
      // to acquire it, so reading them races with the holder publishing them
      // below and clearing them in Mutex_unlock(). Any holder snapshot is stale
      // the instant it is taken anyway; lock_holding() still reports the holder
      // from Mutex_unlock(), where the fields are owned by the caller.
      lock_waiting(location, ahandler);
#ifdef ENABLE_LOCK_LEDGER
      if (lock_ledger::enabled_for(m)) {
        lock_ledger::on_miss(m, location);
      }
#endif
#ifdef LOCK_CONTENTION_PROFILING
      m->unsuccessful_nonblocking_acquires++;
      m->nonblocking_acquires++;
      m->total_acquires++;
      m->print_lock_stats(0);
#endif // LOCK_CONTENTION_PROFILING
#endif // TS_LOCK_SRCLOC
      return false;
    }
    m->thread_holding = t;
#if TS_LOCK_SRCLOC
    m->srcloc  = location;
    m->handler = ahandler;
    if (lock_track_hold_time(m)) {
      m->hold_time = ink_get_hrtime();
    }
#ifdef MAX_LOCK_TAKEN
    m->taken++;
#endif // MAX_LOCK_TAKEN
#endif // TS_LOCK_SRCLOC
#ifdef ENABLE_LOCK_LEDGER
    if (lock_ledger::enabled_for(m)) {
      lock_ledger::on_acquired(m, location, 0, 0);
    }
#endif
  }
#if TS_LOCK_SRCLOC
#ifdef LOCK_CONTENTION_PROFILING
  m->successful_nonblocking_acquires++;
  m->nonblocking_acquires++;
  m->total_acquires++;
  m->print_lock_stats(0);
#endif // LOCK_CONTENTION_PROFILING
#endif // TS_LOCK_SRCLOC
  m->nthread_holding++;
  return true;
}

inline bool
Mutex_trylock(
#if TS_LOCK_SRCLOC
  const SourceLocation &location, const char *ahandler,
#endif
  Ptr<ProxyMutex> &m, EThread *t)
{
  return Mutex_trylock(
#if TS_LOCK_SRCLOC
    location, ahandler,
#endif
    m.get(), t);
}

inline int
Mutex_lock(
#if TS_LOCK_SRCLOC
  const SourceLocation &location, const char *ahandler,
#endif
  ProxyMutex *m, EThread *t)
{
  ink_assert(t != nullptr);
  if (m->thread_holding != t) {
#ifdef ENABLE_LOCK_LEDGER
    ink_hrtime wait_start = 0;
    uint64_t   blocker    = 0;
    if (!lock_ledger::enabled_for(m)) {
      ink_mutex_acquire(&m->the_mutex);
    } else if (!ink_mutex_try_acquire(&m->the_mutex)) {
      wait_start = ink_get_hrtime();
      blocker    = m->ledger_holder.load(std::memory_order_relaxed);
      ink_mutex_acquire(&m->the_mutex);
    }
#else
    ink_mutex_acquire(&m->the_mutex);
#endif
    m->thread_holding = t;
    ink_assert(m->thread_holding);
#if TS_LOCK_SRCLOC
    m->srcloc  = location;
    m->handler = ahandler;
    if (lock_track_hold_time(m)) {
      m->hold_time = ink_get_hrtime();
    }
#ifdef MAX_LOCK_TAKEN
    m->taken++;
#endif // MAX_LOCK_TAKEN
#endif // TS_LOCK_SRCLOC
#ifdef ENABLE_LOCK_LEDGER
    if (lock_ledger::enabled_for(m)) {
      lock_ledger::on_acquired(m, location, wait_start, blocker);
    }
#endif
  }
#if TS_LOCK_SRCLOC
#ifdef LOCK_CONTENTION_PROFILING
  m->blocking_acquires++;
  m->total_acquires++;
  m->print_lock_stats(0);
#endif // LOCK_CONTENTION_PROFILING
#endif // TS_LOCK_SRCLOC
  m->nthread_holding++;
  return true;
}

inline int
Mutex_lock(
#if TS_LOCK_SRCLOC
  const SourceLocation &location, const char *ahandler,
#endif
  Ptr<ProxyMutex> &m, EThread *t)
{
  return Mutex_lock(
#if TS_LOCK_SRCLOC
    location, ahandler,
#endif
    m.get(), t);
}

inline void
Mutex_unlock(ProxyMutex *m, EThread *t)
{
  if (m->nthread_holding) {
    ink_assert(t == m->thread_holding);
    m->nthread_holding--;
    if (!m->nthread_holding) {
#ifdef ENABLE_LOCK_LEDGER
      if (lock_ledger::enabled_for(m)) {
        lock_ledger::on_release(m);
      }
#endif
#ifdef DEBUG
      if (ink_get_hrtime() - m->hold_time > MAX_LOCK_TIME) {
        lock_holding(m->srcloc, m->handler);
      }
#endif
#if TS_LOCK_SRCLOC
#ifdef MAX_LOCK_TAKEN
      if (m->taken > MAX_LOCK_TAKEN)
        lock_taken(m->srcloc, m->handler);
#endif // MAX_LOCK_TAKEN
      m->srcloc  = SourceLocation(nullptr, nullptr, 0);
      m->handler = nullptr;
#endif // TS_LOCK_SRCLOC
      ink_assert(m->thread_holding);
      m->thread_holding = nullptr;
      ink_mutex_release(&m->the_mutex);
    }
  }
}

inline void
Mutex_unlock(Ptr<ProxyMutex> &m, EThread *t)
{
  Mutex_unlock(m.get(), t);
}

class WeakMutexLock
{
private:
  Ptr<ProxyMutex> m;
  bool            locked_p{false};

public:
  WeakMutexLock() = default;

  WeakMutexLock(
#if TS_LOCK_SRCLOC
    const SourceLocation &location, const char *ahandler,
#endif // TS_LOCK_SRCLOC
    Ptr<ProxyMutex> &am, EThread *t)
    : m(am), locked_p(true)
  {
    if (m.get()) {
      Mutex_lock(
#if TS_LOCK_SRCLOC
        location, ahandler,
#endif // TS_LOCK_SRCLOC
        m, t);
    }
  }

  WeakMutexLock(WeakMutexLock &)                  = delete;
  WeakMutexLock &operator=(const WeakMutexLock &) = delete;
  WeakMutexLock(WeakMutexLock &&)                 = delete;

  WeakMutexLock &
  operator=(WeakMutexLock &&orig)
  {
    if (&orig != this) {
      std::swap(m, orig.m);
      std::swap(locked_p, orig.locked_p);
    }
    return *this;
  }

  void
  release()
  {
    if (locked_p && m.get()) {
      Mutex_unlock(m, m->thread_holding);
    }
    locked_p = false;
  }

  ~WeakMutexLock() { this->release(); }
};

/** Scoped lock class for ProxyMutex
 */
class MutexLock
{
private:
  Ptr<ProxyMutex> m{};
  bool            locked_p{false};

public:
  MutexLock() = default;

  MutexLock(
#if TS_LOCK_SRCLOC
    const SourceLocation &location, const char *ahandler,
#endif // TS_LOCK_SRCLOC
    Ptr<ProxyMutex> &am, EThread *t)
    : m(am), locked_p(true)
  {
    Mutex_lock(
#if TS_LOCK_SRCLOC
      location, ahandler,
#endif // TS_LOCK_SRCLOC
      m, t);
  }

  MutexLock(MutexLock &)                  = delete;
  MutexLock &operator=(const MutexLock &) = delete;
  MutexLock(MutexLock &&)                 = delete;

  MutexLock &
  operator=(MutexLock &&orig)
  {
    if (&orig != this) {
      std::swap(m, orig.m);
      std::swap(locked_p, orig.locked_p);
    }
    return *this;
  }

  void
  release()
  {
    if (locked_p) {
      Mutex_unlock(m, m->thread_holding);
    }
    locked_p = false;
  }

  ~MutexLock() { this->release(); }
};

/** Scoped try lock class for ProxyMutex
 */
class WeakMutexTryLock
{
private:
  Ptr<ProxyMutex> m;
  bool            lock_acquired{false};

public:
  WeakMutexTryLock() = default;

  WeakMutexTryLock(
#if TS_LOCK_SRCLOC
    const SourceLocation &location, const char *ahandler,
#endif // TS_LOCK_SRCLOC
    Ptr<ProxyMutex> &am, EThread *t)
    : m(am)
  {
    if (m.get()) {
      lock_acquired = Mutex_trylock(
#if TS_LOCK_SRCLOC
        location, ahandler,
#endif // TS_LOCK_SRCLOC
        m, t);
    } else {
      lock_acquired = true;
    }
  }

  WeakMutexTryLock(WeakMutexTryLock &)                  = delete;
  WeakMutexTryLock &operator=(const WeakMutexTryLock &) = delete;
  WeakMutexTryLock(WeakMutexTryLock &&)                 = delete;

  WeakMutexTryLock &
  operator=(WeakMutexTryLock &&orig)
  {
    if (&orig != this) {
      std::swap(m, orig.m);
      std::swap(lock_acquired, orig.lock_acquired);
    }
    return *this;
  }

  ~WeakMutexTryLock()
  {
    if (lock_acquired && m.get()) {
      Mutex_unlock(m, m->thread_holding);
    }
    lock_acquired = false;
  }

  /** Spin till lock is acquired
   */
  void
  acquire(EThread *t)
  {
    lock_acquired = true;
    if (m.get()) {
      MUTEX_TAKE_LOCK(m, t);
    }
  }

  void
  release()
  {
    if (lock_acquired && m.get()) {
      Mutex_unlock(m, m->thread_holding);
    }
    lock_acquired = false;
  }

  bool
  is_locked() const
  {
    return lock_acquired;
  }

  const ProxyMutex *
  get_mutex() const
  {
    return m.get();
  }
};

/** Scoped try lock class for ProxyMutex
 */
class MutexTryLock
{
private:
  Ptr<ProxyMutex> m;
  bool            lock_acquired{false};

public:
  MutexTryLock() = default;

  MutexTryLock(
#if TS_LOCK_SRCLOC
    const SourceLocation &location, const char *ahandler,
#endif // TS_LOCK_SRCLOC
    Ptr<ProxyMutex> &am, EThread *t)
    : m(am)
  {
    lock_acquired = Mutex_trylock(
#if TS_LOCK_SRCLOC
      location, ahandler,
#endif // TS_LOCK_SRCLOC
      m, t);
  }

  MutexTryLock(MutexTryLock &)                  = delete;
  MutexTryLock &operator=(const MutexTryLock &) = delete;
  MutexTryLock(MutexTryLock &&)                 = delete;

  MutexTryLock &
  operator=(MutexTryLock &&orig)
  {
    if (&orig != this) {
      std::swap(m, orig.m);
      std::swap(lock_acquired, orig.lock_acquired);
    }
    return *this;
  }

  ~MutexTryLock()
  {
    if (lock_acquired) {
      Mutex_unlock(m, m->thread_holding);
    }
  }

  /** Spin till lock is acquired
   */
  void
  acquire(EThread *t)
  {
    MUTEX_TAKE_LOCK(m, t);
    lock_acquired = true;
  }

  void
  release()
  {
    if (lock_acquired) {
      Mutex_unlock(m, m->thread_holding);
    }
    lock_acquired = false;
  }

  bool
  is_locked() const
  {
    return lock_acquired;
  }

  const ProxyMutex *
  get_mutex() const
  {
    return m.get();
  }
};

inline void
ProxyMutex::free()
{
#if TS_LOCK_SRCLOC
#ifdef LOCK_CONTENTION_PROFILING
  print_lock_stats(1);
#endif
#endif
  ink_mutex_destroy(&the_mutex);
  mutexAllocator.free(this);
}

// TODO should take optional mutex "name" identifier, to pass along to the init() fun
/**
  Creates a new ProxyMutex object.

  This is the preferred mechanism for constructing objects of the
  ProxyMutex class. It provides you with faster allocation than
  that of the normal constructor.

  @return A pointer to a ProxyMutex object appropriate for the build
    environment.

*/
inline ProxyMutex *
new_ProxyMutex()
{
  ProxyMutex *m = mutexAllocator.alloc();
  m->init();
  return m;
}
