"""
Test that early POST returns don't allow smuggle attacks
"""
#  Licensed to the Apache Software Foundation (ASF) under one
#  or more contributor license agreements.  See the NOTICE file
#  distributed with this work for additional information
#  regarding copyright ownership.  The ASF licenses this file
#  to you under the Apache License, Version 2.0 (the
#  "License"); you may not use this file except in compliance
#  with the License.  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.


Test.testName = "post_request_smuggle"

ts = Test.MakeATSProcess("ts")
ts.Disk.records_config.update({
    'proxy.config.diags.debug.enabled': 1,
    'proxy.config.diags.debug.tags': 'cache|http|dns|hostdb|parent_proxy',
})

tr = Test.AddTestRun("smuggle attack")
replay_file = "replay/post_request_smuggle.replay.yaml"
server = tr.AddVerifierServerProcess(
    "server",
    replay_file
)

tr.AddVerifierClientProcess(
    'client',
    replay_file,
    http_ports=[ts.Variables.port]
)
ts.Disk.remap_config.AddLine(
    f'map / http://localhost:{server.Variables.http_port}'
)
tr.Processes.Default.StartBefore(ts)
tr.StillRunningAfter = ts
