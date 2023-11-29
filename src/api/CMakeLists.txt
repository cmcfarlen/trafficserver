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

# plugin api *only*
add_library(tsapi SHARED InkAPI.cc InkIOCoreAPI.cc)
add_library(ts::tsapi ALIAS tsapi)

set(TSAPI_PUBLIC_HEADERS
    ${PROJECT_SOURCE_DIR}/include/ts/ts.h
    ${PROJECT_SOURCE_DIR}/include/ts/remap.h
    ${PROJECT_SOURCE_DIR}/include/ts/TsException.h
    ${PROJECT_SOURCE_DIR}/include/ts/parentselectdefs.h
    ${PROJECT_SOURCE_DIR}/include/ts/parentselectdefs.h
    ${PROJECT_BINARY_DIR}/include/ts/apidefs.h
    ${PROJECT_SOURCE_DIR}/include/ts/experimental.h
)

target_link_libraries(tsapi PRIVATE libswoc::libswoc yaml-cpp::yaml-cpp PCRE::PCRE OpenSSL::SSL)
set_target_properties(tsapi PROPERTIES PUBLIC_HEADER "${TSAPI_PUBLIC_HEADERS}")
target_include_directories(tsapi INTERFACE "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# Items common between api and other ts libraries
add_library(
  tsapibackend
  ConfigUpdateCbTable.cc # configmanager
  InkContInternal.cc # proxy, http
  InkVConnInternal.cc # proxy, http
  APIHook.cc # iocore
  APIHooks.cc # proxy
  LifecycleAPIHooks.cc # proxy, http
  HttpHookState.cc # proxy, http
)
add_library(ts::tsapibackend ALIAS tsapibackend)
target_link_libraries(
  tsapibackend
  PUBLIC ts::inknet ts::http
  PRIVATE ts::proxy ts::hdrs ts::tscore
)

# Items that even tscore can depend on
set(TSAPICORE_PUBLIC_HEADERS
    ${PROJECT_SOURCE_DIR}/include/api/Metrics.h
    ${PROJECT_SOURCE_DIR}/include/api/SourceLocation.h
    ${PROJECT_SOURCE_DIR}/include/api/DbgCtl.h
    ${PROJECT_SOURCE_DIR}/include/api/APIHook.h
    ${PROJECT_SOURCE_DIR}/include/api/APIHooks.h
    ${PROJECT_SOURCE_DIR}/include/api/FeatureAPIHooks.h
    ${PROJECT_SOURCE_DIR}/include/api/InkAPIInternal.h
    ${PROJECT_SOURCE_DIR}/include/api/LifecycleAPIHooks.h
    ${PROJECT_SOURCE_DIR}/include/api/Metrics.h
    ${PROJECT_SOURCE_DIR}/include/api/SourceLocation.h
    ${PROJECT_SOURCE_DIR}/include/api/ts_bw_format.h
    ${PROJECT_SOURCE_DIR}/include/api/ts_diag_levels.h
    ${PROJECT_SOURCE_DIR}/include/api/ts_errata.h
)
add_library(tsapicore SHARED Assert.cc Metrics.cc DbgCtl.cc SourceLocation.cc ts_diags.cc)
add_library(ts::tsapicore ALIAS tsapicore)
target_link_libraries(tsapicore PRIVATE ts::tscore libswoc::libswoc)

set(TSAPICORE_PUBLIC_HEADERS ${PROJECT_SOURCE_DIR}/include/api/Metrics.h
                             ${PROJECT_SOURCE_DIR}/include/ts/DbgCtl.h # move this guy to include/api prolly
)

set_target_properties(tsapicore PROPERTIES POSITION_INDEPENDENT_CODE TRUE PUBLIC_HEADER "${TSAPICORE_PUBLIC_HEADERS}")

install(
  TARGETS tsapi
  EXPORT tsapi-targets
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ts
)
install(
  TARGETS tsapibackend
  EXPORT tsapibackend-targets
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ts
)
install(
  TARGETS tsapicore
  EXPORT tsapicore-targets
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/api
)

install(
  EXPORT tsapi-targets
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tsapi
  NAMESPACE ts::
)
install(
  EXPORT tsapibackend-targets
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tsapibackend
  NAMESPACE ts::
)
install(
  EXPORT tsapicore-targets
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tsapicore
  NAMESPACE ts::
)

if(APPLE)
  target_link_options(tsapi PRIVATE -undefined dynamic_lookup)
endif()

if(BUILD_TESTING)
  add_executable(test_ApiCore unit_tests/test_Metrics.cc)

  target_link_libraries(test_ApiCore PRIVATE tscore tsapicore catch2::catch2)

  add_test(NAME test_ApiCore COMMAND $<TARGET_FILE:test_ApiCore>)
endif()
