#######################
#
#  Licensed to the Apache Software Foundation (ASF) under one or more contributor license
#  agreements.  See the NOTICE file distributed with this work for additional information regarding
#  copyright ownership.  The ASF licenses this file to you under the Apache License, Version 2.0
#  (the "License"); you may not use this file except in compliance with the License.  You may obtain
#  a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software distributed under the License
#  is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
#  or implied. See the License for the specific language governing permissions and limitations under
#  the License.
#
#######################

add_atsplugin(ja4_fingerprint ja4.cc plugin.cc tls_client_hello_summary.cc)
target_link_libraries(ja4_fingerprint PRIVATE OpenSSL::Crypto OpenSSL::SSL)
verify_global_plugin(ja4_fingerprint)

if(BUILD_TESTING)
  add_executable(test_ja4 test_ja4.cc ja4.cc tls_client_hello_summary.cc)
  target_link_libraries(test_ja4 PRIVATE catch2::catch2)

  add_test(test_ja4 test_ja4)
endif()
