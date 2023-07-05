# libts Makefile.am
#
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

include $(top_srcdir)/mk/tidy.mk

# check_PROGRAMS = test_api

# TESTS_ENVIRONMENT = LSAN_OPTIONS=suppressions=$(abs_top_srcdir)/ci/asan_leak_suppression/unit_tests.txt
# TESTS = $(check_PROGRAMS)

AM_CPPFLAGS += \
	@SWOC_INCLUDES@ \
	$(TS_INCLUDES)

noinst_LIBRARIES = libtsapi.a

libtsapi_a_SOURCES = \
	Metrics.cc

# test_tsapi_CPPFLAGS = $(AM_CPPFLAGS)\
# 	-I$(abs_top_srcdir)/lib/catch2

# test_tsapi_CXXFLAGS = -Wno-array-bounds $(AM_CXXFLAGS)
# test_tsapi_LDADD = libtsapi.la @SWOC_LIBS@
# test_tsapi_SOURCES = \
# 	unit_tests/test_Metrics.cc

# clean-local:
# 	rm -f ParseRulesCType ParseRulesCTypeToLower ParseRulesCTypeToUpper


clang-tidy-local: $(DIST_SOURCES)
	$(CXX_Clang_Tidy)
