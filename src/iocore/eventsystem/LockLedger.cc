/** @file

  Lock ledger: per-thread recording of ProxyMutex contention.

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

#include "iocore/eventsystem/Lock.h"

#ifdef ENABLE_LOCK_LEDGER

#include "tscore/ink_thread.h"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/uio.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace lock_ledger
{
namespace
{
  // On-disk layout; tools/lock_ledger/lock_ledger.py must match.
  constexpr char     FILE_MAGIC[4]  = {'L', 'K', 'L', '2'};
  constexpr char     CHUNK_MAGIC[4] = {'C', 'H', 'N', 'K'};
  constexpr uint32_t FILE_VERSION   = 1;

  enum Op : uint16_t {
    OP_MISS    = 1, ///< Try-lock failed; other_* is the holder seen at that moment.
    OP_WAIT    = 2, ///< Blocking acquire had to wait @c duration ns; other_* is the holder when the wait began.
    OP_RELEASE = 3, ///< Outermost unlock; @c site is where it was acquired and @c duration the hold time.
    OP_PARKED  = 4, ///< The loop took the mutex for a parked Event; @c duration is how long it was parked.
  };

  struct Entry {
    int64_t  timestamp; ///< ink_get_hrtime() ns
    uint64_t mutex;
    uint64_t context;
    uint32_t site;
    uint32_t other_site;
    uint32_t duration; ///< ns, saturated
    uint16_t op;
    uint16_t other_thread;
  };
  static_assert(sizeof(Entry) == 40);

  constexpr size_t     CHUNK_ENTRIES  = 32768;
  constexpr size_t     MAX_QUEUED     = 128;
  constexpr ink_hrtime FLUSH_INTERVAL = HRTIME_SECOND;

  enum class Mode { OFF, NAMED, ALL };

  Mode
  mode_from_env()
  {
    const char *v = getenv("TS_LOCK_LEDGER");
    if (v == nullptr) {
      return Mode::NAMED;
    }
    if (strcmp(v, "all") == 0) {
      return Mode::ALL;
    }
    if (strcmp(v, "off") == 0) {
      return Mode::OFF;
    }
    return Mode::NAMED;
  }

  const Mode mode = mode_from_env();

  inline uint64_t
  pack_holder(uint32_t thread, uint32_t site)
  {
    return (static_cast<uint64_t>(thread) << 32) | site;
  }

  inline uint32_t
  saturate(ink_hrtime ns)
  {
    if (ns <= 0) {
      return 0;
    }
    return ns > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ns);
  }

  template <typename T>
  void
  put(std::vector<char> &buf, T v)
  {
    const char *p = reinterpret_cast<const char *>(&v);
    buf.insert(buf.end(), p, p + sizeof(v));
  }

  void
  put_str(std::vector<char> &buf, std::string_view s)
  {
    uint16_t n = s.size() > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(s.size());
    put(buf, n);
    buf.insert(buf.end(), s.data(), s.data() + n);
  }

  struct Chunk {
    uint32_t           thread;
    std::string        thread_name;
    std::vector<char>  defs;
    std::vector<Entry> entries;
  };

  /// Writes chunks from every thread on its own thread so recording threads never block on file I/O.
  class Writer
  {
  public:
    Writer() : _dir(getenv("TS_LOCK_LEDGER_DIR") ? getenv("TS_LOCK_LEDGER_DIR") : "/tmp"), _thread([this] { run(); }) {}

    /// Drains queued chunks and stops the writer thread. Chunks submitted afterward are discarded.
    void
    shutdown()
    {
      {
        std::lock_guard g(_mtx);
        if (_stop) {
          return;
        }
        _stop = true;
      }
      _cv.notify_one();
      _thread.join();
      for (auto &[tid, fd] : _fds) {
        close(fd);
      }
    }

    void
    submit(std::unique_ptr<Chunk> c)
    {
      {
        std::lock_guard g(_mtx);
        if (_stop) {
          return;
        }
        if (_queue.size() >= MAX_QUEUED) {
          ++_dropped;
          return;
        }
        _queue.push_back(std::move(c));
      }
      _cv.notify_one();
    }

  private:
    void
    run()
    {
      std::unique_lock lk(_mtx);
      for (;;) {
        _cv.wait(lk, [this] { return _stop || !_queue.empty(); });
        if (_queue.empty()) {
          return;
        }
        auto     c       = std::move(_queue.front());
        uint32_t dropped = _dropped;
        _queue.pop_front();
        lk.unlock();
        write(*c, dropped);
        lk.lock();
      }
    }

    void
    write(const Chunk &c, uint32_t dropped)
    {
      int fd = open_for(c);
      if (fd < 0) {
        return;
      }
      std::vector<char> hdr;
      hdr.insert(hdr.end(), CHUNK_MAGIC, CHUNK_MAGIC + 4);
      put(hdr, static_cast<uint32_t>(c.defs.size()));
      put(hdr, static_cast<uint32_t>(c.entries.size()));
      put(hdr, dropped);
      iovec v[3] = {
        {hdr.data(),                            hdr.size()                      },
        {const_cast<char *>(c.defs.data()),     c.defs.size()                   },
        {const_cast<Entry *>(c.entries.data()), c.entries.size() * sizeof(Entry)},
      };
      if (::writev(fd, v, 3) < 0) {
        Warning("lock ledger: write failed for thread %u: %s", c.thread, strerror(errno));
      }
    }

    int
    open_for(const Chunk &c)
    {
      if (auto it = _fds.find(c.thread); it != _fds.end()) {
        return it->second;
      }
      std::string path = _dir + "/lockledger." + std::to_string(getpid()) + "." + std::to_string(c.thread) + ".bin";
      int         fd   = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
      if (fd < 0) {
        Warning("lock ledger: cannot open %s: %s", path.c_str(), strerror(errno));
      } else {
        std::vector<char> hdr;
        hdr.insert(hdr.end(), FILE_MAGIC, FILE_MAGIC + 4);
        put(hdr, FILE_VERSION);
        put(hdr, static_cast<uint32_t>(getpid()));
        put(hdr, c.thread);
        put_str(hdr, c.thread_name);
        if (::write(fd, hdr.data(), hdr.size()) < 0) {
          Warning("lock ledger: header write failed for %s: %s", path.c_str(), strerror(errno));
        }
      }
      _fds.emplace(c.thread, fd);
      return fd;
    }

    std::string                        _dir;
    std::mutex                         _mtx;
    std::condition_variable            _cv;
    std::deque<std::unique_ptr<Chunk>> _queue;
    std::unordered_map<uint32_t, int>  _fds;
    uint32_t                           _dropped = 0;
    bool                               _stop    = false;
    std::thread                        _thread;
  };

  // Never destroyed: event threads keep recording while the process exits, so they must not touch a destructed object.
  Writer &
  writer()
  {
    static Writer *w = [] {
      auto *p = new Writer;
      atexit([] { writer().shutdown(); });
      return p;
    }();
    return *w;
  }

  /// Call sites deduplicated by content: __FILE__ literals for an inline header function differ per translation unit.
  class SiteRegistry
  {
  public:
    uint32_t
    intern(const SourceLocation &loc, bool &is_new)
    {
      std::lock_guard g(_mtx);
      auto [it, inserted] =
        _ids.try_emplace(Key{loc.file ? loc.file : "", loc.func ? loc.func : "", loc.line}, static_cast<uint32_t>(_ids.size() + 1));
      is_new = inserted;
      return it->second;
    }

  private:
    using Key = std::tuple<std::string, std::string, int>;
    struct KeyHash {
      size_t
      operator()(const Key &k) const
      {
        return std::hash<std::string>()(std::get<0>(k)) ^ (std::hash<std::string>()(std::get<1>(k)) << 1) ^ std::get<2>(k);
      }
    };
    std::mutex                                 _mtx;
    std::unordered_map<Key, uint32_t, KeyHash> _ids;
  };

  SiteRegistry          sites;
  std::atomic<uint32_t> next_thread_id{1};

  class ThreadLedger
  {
  public:
    ThreadLedger() : _id(next_thread_id++)
    {
      char name[MAX_THREAD_NAME_LENGTH] = {};
      ink_get_thread_name(name, sizeof(name));
      _name = name;
      reset();
    }

    ~ThreadLedger() { flush(); }

    uint32_t
    id() const
    {
      return _id;
    }

    /// Site ids are global; each thread defines the sites it uses in its own file so every file is self-describing.
    uint32_t
    site_for(const SourceLocation &loc)
    {
      PtrKey key{loc.file, loc.func, loc.line};
      if (auto it = _site_cache.find(key); it != _site_cache.end()) {
        return it->second;
      }
      bool     is_new = false;
      uint32_t id     = sites.intern(loc, is_new);
      _site_cache.emplace(key, id);
      _chunk->defs.push_back('S');
      put(_chunk->defs, id);
      put(_chunk->defs, static_cast<uint32_t>(loc.line));
      put_str(_chunk->defs, loc.file ? loc.file : "");
      put_str(_chunk->defs, loc.func ? loc.func : "");
      return id;
    }

    void
    define_mutex(const ProxyMutex *m, const char *name)
    {
      _chunk->defs.push_back('M');
      put(_chunk->defs, reinterpret_cast<uint64_t>(m));
      put_str(_chunk->defs, name);
    }

    void
    emit(const Entry &e)
    {
      _chunk->entries.push_back(e);
      if (_chunk->entries.size() >= CHUNK_ENTRIES || e.timestamp - _last_flush > FLUSH_INTERVAL) {
        flush();
        _last_flush = e.timestamp;
      }
    }

    void
    flush()
    {
      if (_chunk->entries.empty() && _chunk->defs.empty()) {
        return;
      }
      writer().submit(std::move(_chunk));
      reset();
    }

  private:
    struct PtrKey {
      const char *file;
      const char *func;
      int         line;
      bool        operator==(const PtrKey &) const = default;
    };
    struct PtrKeyHash {
      size_t
      operator()(const PtrKey &k) const
      {
        return std::hash<const void *>()(k.file) ^ (std::hash<const void *>()(k.func) << 1) ^ k.line;
      }
    };

    void
    reset()
    {
      _chunk              = std::make_unique<Chunk>();
      _chunk->thread      = _id;
      _chunk->thread_name = _name;
      _chunk->entries.reserve(CHUNK_ENTRIES);
    }

    uint32_t                                         _id;
    std::string                                      _name;
    std::unique_ptr<Chunk>                           _chunk;
    std::unordered_map<PtrKey, uint32_t, PtrKeyHash> _site_cache;
    ink_hrtime                                       _last_flush = ink_get_hrtime();
  };

  ThreadLedger &
  local()
  {
    thread_local ThreadLedger tl;
    return tl;
  }

  Entry
  make_entry(Op op, const ProxyMutex *m, int64_t ts, uint32_t site)
  {
    Entry e{};
    e.timestamp = ts;
    e.mutex     = reinterpret_cast<uint64_t>(m);
    e.context   = current_context;
    e.site      = site;
    e.op        = op;
    return e;
  }

} // namespace

bool                   record_all      = mode == Mode::ALL;
thread_local uintptr_t current_context = 0;

void
name(ProxyMutex *m, const char *name)
{
  if (mode == Mode::OFF) {
    return;
  }
  m->ledger_name = name;
  local().define_mutex(m, name);
}

void
on_miss(ProxyMutex *m, const SourceLocation &loc)
{
  auto    &tl     = local();
  uint64_t holder = m->ledger_holder.load(std::memory_order_relaxed);
  Entry    e      = make_entry(OP_MISS, m, ink_get_hrtime(), tl.site_for(loc));
  e.other_site    = static_cast<uint32_t>(holder);
  e.other_thread  = static_cast<uint16_t>(holder >> 32);
  tl.emit(e);
}

void
on_acquired(ProxyMutex *m, const SourceLocation &loc, ink_hrtime wait_start, uint64_t blocker)
{
  auto    &tl   = local();
  uint32_t site = tl.site_for(loc);
  m->ledger_holder.store(pack_holder(tl.id(), site), std::memory_order_relaxed);
  if (wait_start != 0) {
    Entry e        = make_entry(OP_WAIT, m, wait_start, site);
    e.duration     = saturate(m->hold_time - wait_start);
    e.other_site   = static_cast<uint32_t>(blocker);
    e.other_thread = static_cast<uint16_t>(blocker >> 32);
    tl.emit(e);
  }
}

void
on_parked(ProxyMutex *m, const SourceLocation &loc, ink_hrtime since)
{
  auto   &tl  = local();
  int64_t now = ink_get_hrtime();
  Entry   e   = make_entry(OP_PARKED, m, now, tl.site_for(loc));
  e.duration  = saturate(now - since);
  tl.emit(e);
}

void
on_release(ProxyMutex *m)
{
  uint64_t holder = m->ledger_holder.load(std::memory_order_relaxed);
  if (holder == 0) {
    // Acquired before the mutex was opted in.
    return;
  }
  m->ledger_holder.store(0, std::memory_order_relaxed);
  int64_t now = ink_get_hrtime();
  Entry   e   = make_entry(OP_RELEASE, m, now, static_cast<uint32_t>(holder));
  e.duration  = saturate(now - m->hold_time);
  local().emit(e);
}

} // namespace lock_ledger

#endif // ENABLE_LOCK_LEDGER
