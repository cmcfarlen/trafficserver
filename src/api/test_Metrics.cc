/** @file

    TextView unit tests.

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

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "api/Metrics.h"

TEST_CASE("Metrics", "[libtsapi][Metrics]")
{
  ts::Metrics m;

  auto fooid = m.newMetric("foo");

  REQUIRE(fooid == 0);

  m.increment(fooid);

  m.recordsDump([](RecT, void *, int, const char *name, int value, RecData *) { printf("Fooo: %s: %d\n", name, value); }, nullptr);
}
