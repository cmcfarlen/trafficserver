/** @file

  A brief file description

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

#pragma once

#include <array>
#include <unordered_map>
#include <tuple>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

#include "records/P_RecDefs.h"
#include "ts/apidefs.h"

namespace ts
{
class Metrics
{
private:
  using self_type  = Metrics;
  using IdType     = int32_t; // Could be a tuple, but one way or another, they have to be combined to an int32_t.
  using AtomicType = std::atomic<int64_t>;

public:
  static constexpr uint16_t METRICS_MAX_BLOBS = 8192;
  static constexpr uint16_t METRICS_MAX_SIZE  = 2048;                               // For a total of 16M metrics
  static constexpr IdType NOT_FOUND           = std::numeric_limits<IdType>::min(); // <16-bit,16-bit> = <blob-index,offset>

private:
  using NameAndId       = std::tuple<std::string, IdType>;
  using NameContainer   = std::array<NameAndId, METRICS_MAX_SIZE>;
  using AtomicContainer = std::array<AtomicType, METRICS_MAX_SIZE>;
  using MetricStorage   = std::tuple<NameContainer, AtomicContainer>;
  using MetricBlobs     = std::array<MetricStorage *, METRICS_MAX_BLOBS>;
  using LookupTable     = std::unordered_map<std::string_view, IdType>;

public:
  Metrics(const self_type &)              = delete;
  self_type &operator=(const self_type &) = delete;
  Metrics &operator=(Metrics &&)          = delete;
  Metrics(Metrics &&)                     = delete;

  virtual ~Metrics() = default;

  Metrics()
  {
    _blobs[0] = new MetricStorage();
    ink_release_assert(_blobs[0]);
    ink_release_assert(0 == newMetric("proxy.node.bad_id.metrics")); // Reserve slot 0 for errors, this should always be 0
  }

  // Singleton
  static Metrics &getInstance();

  // Yes, we don't return objects here, but rather ID's and atomic's directly. We could
  // make these objects, but that's a lot of object instances to manage, and right now the
  // ID in the containers is very small and sufficient to work with. But agreed, it's not
  // very C++-like (but compatible with old librecords ATS code!).
  IdType newMetric(const std::string_view name);
  IdType lookup(const std::string_view name) const;
  AtomicType *lookup(IdType id) const;

  AtomicType &
  operator[](IdType id)
  {
    return *lookup(id);
  }

  IdType
  operator[](const std::string_view name) const
  {
    return lookup(name);
  }

  // Inlined, for performance
  int64_t
  increment(IdType id, uint64_t val = 1)
  {
    auto metric = lookup(id);

    return (metric ? metric->fetch_add(val) : NOT_FOUND);
  }

  int64_t
  decrement(IdType id, uint64_t val = 1)
  {
    auto metric = lookup(id);

    return (metric ? metric->fetch_sub(val) : NOT_FOUND);
  }

  int64_t
  get(IdType id) const
  {
    auto metric = lookup(id);

    return (metric ? metric->load() : NOT_FOUND);
  }

  std::string_view get_name(IdType id) const;

  void
  set(IdType id, int64_t val)
  {
    auto metric = lookup(id);

    if (metric) {
      metric->store(val);
    }
  }

  bool
  valid(IdType id) const
  {
    auto [blob, entry] = _splitID(id);

    return (id >= 0 && blob <= _cur_blob && entry < METRICS_MAX_SIZE);
  }

  void recordsDump(RecDumpEntryCb callback, void *edata) const;

  class iterator
  {
  public:
    using iterator_category = std::input_iterator_tag;
    using value_type        = std::pair<std::string_view, AtomicType::value_type>;
    using difference_type   = ptrdiff_t;
    using pointer           = value_type *;
    using reference         = value_type &;

    iterator(const Metrics &m, IdType pos) : metrics(m), it(pos) {}

    iterator &
    operator++()
    {
      next();
      return *this;
    }

    iterator
    operator++(int)
    {
      iterator result = *this;
      next();
      return result;
    }

    value_type
    operator*() const
    {
      return std::make_pair(metrics.get_name(it), metrics.lookup(it)->load());
    }

    bool
    operator==(const iterator &o) const
    {
      return std::addressof(metrics) == std::addressof(o.metrics) && it == o.it;
    }
    bool
    operator!=(const iterator &o) const
    {
      return std::addressof(metrics) != std::addressof(o.metrics) || it != o.it;
    }

  private:
    void
    next()
    {
      auto [blob, offset] = metrics._splitID(it);

      if (++offset == METRICS_MAX_SIZE) {
        ++blob;
        offset = 0;
      }

      it = _makeId(blob, offset);
    }

    const Metrics &metrics;
    IdType it;
  };

  iterator
  begin() const
  {
    return iterator(*this, 0);
  }

  iterator
  end() const
  {
    _mutex.lock();
    int16_t blob   = _cur_blob;
    int16_t offset = _cur_off;
    _mutex.unlock();
    return iterator(*this, _makeId(blob, offset));
  }

private:
  static constexpr std::tuple<uint16_t, uint16_t>
  _splitID(IdType value)
  {
    return std::make_tuple(static_cast<uint16_t>(value >> 16), static_cast<uint16_t>(value & 0xFFFF));
  }

  static constexpr IdType
  _makeId(uint16_t blob, uint16_t offset)
  {
    return (blob << 16 | offset);
  }

  void _addBlob();

  mutable std::mutex _mutex;
  LookupTable _lookups;
  MetricBlobs _blobs;
  uint16_t _cur_blob = 0;
  uint16_t _cur_off  = 0;
}; // class Metrics

} // namespace ts
