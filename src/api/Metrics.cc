/** @file

  The implementations of the Metrics API class.

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

#include "ts/ts.h"
#include "api/Metrics.h"

namespace ts
{

// This is the singleton instance of the metrics class.
Metrics &
Metrics::getInstance()
{
  static ts::Metrics _instance;

  return _instance;
}

void
Metrics::_addBlob() // The mutex must be held before calling this!
{
  auto blob = new Metrics::MetricStorage();

  ink_assert(blob);
  ink_assert(_cur_blob < Metrics::METRICS_MAX_BLOBS);

  _blobs[++_cur_blob] = blob;
  _cur_off            = 0;
}

Metrics::IdType
Metrics::newMetric(std::string_view name)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _lookups.find(name);

  if (it != _lookups.end()) {
    return it->second;
  }

  Metrics::IdType id                = (_cur_blob << 16 | _cur_off);
  Metrics::MetricStorage *blob      = _blobs[_cur_blob];
  Metrics::NameContainer &names     = std::get<0>(*blob);
  Metrics::AtomicContainer &atomics = std::get<1>(*blob);

  atomics[_cur_off].store(0);
  names[_cur_off] = std::make_tuple(std::string(name), id);
  _lookups.emplace(std::get<0>(names[_cur_off]), id);

  if (++_cur_off >= Metrics::METRICS_MAX_SIZE) {
    _addBlob(); // This resets _cur_off to 0 as well
  }

  return id;
}

Metrics::IdType
Metrics::lookup(const std::string_view name)
{
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _lookups.find(name);

  if (it != _lookups.end()) {
    return it->second;
  }

  return NOT_FOUND;
}

Metrics::AtomicType *
Metrics::lookup(IdType id) const
{
  std::tuple<uint16_t, uint16_t> idx = _splitID(id);
  Metrics::MetricStorage *blob       = _blobs[std::get<0>(idx)];
  Metrics::AtomicContainer &atomics  = std::get<1>(*blob);

  return &(atomics[std::get<1>(idx)]);
}

// ToDo: This is probably not great, and likely we should have some better
// way exposing iterators over the Metrics. That avoids this ugly dependency
// between librecords and this code.
void
Metrics::recordsDump(RecDumpEntryCb callback, void *edata)
{
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
      callback(RECT_PLUGIN, edata, 1, std::get<0>(names[j]).c_str(), TS_RECORDDATATYPE_INT, &datum);
    }
  }
}

} // namespace ts
