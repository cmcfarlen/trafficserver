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
Metrics::lookup(const std::string_view name) const
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
  auto [blob_ix, offset]       = _splitID(id);
  Metrics::MetricStorage *blob = _blobs[blob_ix];

  // Do a sanity check on the ID, to make sure we don't index outside of the realm of possibility.
  if (!blob || (blob_ix == _cur_blob && offset > _cur_off)) {
    blob   = _blobs[0];
    offset = 0;
  }

  return &((std::get<1>(*blob)[offset]));
}

std::string_view
Metrics::get_name(Metrics::IdType id) const
{
  auto [blob_ix, offset]       = _splitID(id);
  Metrics::MetricStorage *blob = _blobs[blob_ix];

  // Do a sanity check on the ID, to make sure we don't index outside of the realm of possibility.
  if (!blob || (blob_ix == _cur_blob && offset > _cur_off)) {
    blob   = _blobs[0];
    offset = 0;
  }

  const std::string &result = std::get<0>(std::get<0>(*blob)[offset]);
  return result;
}

// ToDo: While this is using iterators now, there is this unfortunate use of .data()
// that should probably go away.  I had problems using const std::string& in the iterator
// value_type so this needs to be revisited.
void
Metrics::recordsDump(RecDumpEntryCb callback, void *edata) const
{
  RecData datum;

  for (auto [name, metric] : *this) {
    datum.rec_int = metric;
    callback(RECT_PLUGIN, edata, 1, name.data(), TS_RECORDDATATYPE_INT, &datum);
  }
}

} // namespace ts
