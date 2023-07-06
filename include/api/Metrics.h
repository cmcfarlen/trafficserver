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
  using IdType     = int32_t;
  using AtomicType = std::atomic<int64_t>;

public:
  static constexpr uint16_t METRICS_MAX_BLOBS = 8192;
  static constexpr uint16_t METRICS_MAX_SIZE  = 2048;      // For a total of 16M metrics
  static constexpr IdType NOT_FOUND           = INT32_MIN; // <16-bit,16-bit> = <blob-index,offset>

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

  Metrics() { _blobs[0] = new MetricStorage(); }

  // Singleton
  static Metrics &getInstance();

  IdType newMetric(const std::string_view name);
  IdType lookup(const std::string_view name);
  AtomicType *lookup(IdType id) const;

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
    std::tuple<uint16_t, uint16_t> idx = _splitID(id);

    return (id >= 0 && std::get<0>(idx) < _cur_blob && std::get<1>(idx) < METRICS_MAX_SIZE);
  }

  void
  recordsDump(RecDumpEntryCb callback, void *edata)
  {
    return;
    int16_t blob_ix, off_ix;
    int16_t off_max = METRICS_MAX_SIZE;

    {
      std::lock_guard<std::mutex> lock(_mutex);

      // Capture these while protected, in case the blobs change
      blob_ix = _cur_blob;
      off_ix  = _cur_off;
    }

    for (int i = 0; i <= blob_ix; ++i) {
      auto blob     = _blobs[i];
      auto &names   = std::get<0>(*blob);
      auto &metrics = std::get<1>(*blob);
      RecData datum;

      if (i == blob_ix) {
        off_max = off_ix;
      }
      for (int j = 0; j < off_max; ++j) {
        datum.rec_int = metrics[j].load();
        // ToDo: The recordtype here is fine for now, but we should probably make this generic
        callback(RECT_PLUGIN, edata, 1, std::get<0>(names[i]).c_str(), TS_RECORDDATATYPE_INT, &datum);
      }
    }
  }

private:
  static constexpr std::tuple<uint16_t, uint16_t>
  _splitID(IdType value)
  {
    return std::make_tuple(static_cast<uint16_t>(value >> 16), static_cast<uint16_t>(value & 0xFFFF));
  }

  void _addBlob();

  std::mutex _mutex;
  LookupTable _lookups;
  MetricBlobs _blobs;
  uint16_t _cur_blob = 0;
  uint16_t _cur_off  = 0;
}; // class Metrics

} // namespace ts
