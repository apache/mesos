# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License

ACLOCAL_AMFLAGS = -I m4

AUTOMAKE_OPTIONS = foreign

# If we try and include third_party libraries as subdirs then they
# will also try to get installed, which we don't want. Instead, we
# build . first so that the all-local and clean-local hooks in this
# file get run before we build anything else. This ensures third_party
# libraries get built (and cleaned) first without worrying about them
# getting installed.
SUBDIRS = . src

# While we don't want to install third_party, make sure we include it
# in distributions.
DIST_SUBDIRS = $(subdirs) src

PROTOBUF = third_party/protobuf-2.3.0
GLOG = third_party/glog-0.3.1
GMOCK = third_party/gmock-1.6.0
GTEST = $(GMOCK)/gtest
ZOOKEEPER = third_party/zookeeper-3.3.1/src/c
LIBPROCESS = third_party/libprocess
LEVELDB = third_party/leveldb
SETUPTOOLS = third_party/distribute-0.6.19

EXTRA_DIST = support/colors.sh

# Build the gmock library for tests (check target).
check_LTLIBRARIES = libgmock.la
libgmock_la_SOURCES = $(top_srcdir)/$(GMOCK)/src/gmock-all.cc	\
                      $(top_srcdir)/$(GTEST)/src/gtest-all.cc
libgmock_la_CPPFLAGS = -I$(top_srcdir)/$(GTEST)/include         \
                       -I$(top_srcdir)/$(GMOCK)/include         \
                       -I$(top_srcdir)/$(GTEST)                 \
                       -I$(top_srcdir)/$(GMOCK)

# Since we generate several files in src/ with config.status, make
# sure they're regenerated before we recurse into the src directory.
all-recursive: src/python/setup.py src/java/mesos.pom

# Most of our third party libraries support VPATH builds so we can
# simply invoke make on them provided they have been configured
# properly in our configure.ac. Unfortunately, this is not the case
# with leveldb, so we copy the entire directory before trying to build
# it (ugh). Likewise, we need to copy the Python setuptools egg that
# we ship as well.

all-local:
	@echo "Building third-party libraries"
	@cd $(PROTOBUF) && $(MAKE) $(AM_MAKEFLAGS) all
	@cd $(GLOG) && $(MAKE) $(AM_MAKEFLAGS) all
if WITH_INCLUDED_ZOOKEEPER
	@cd $(ZOOKEEPER) && $(MAKE) $(AM_MAKEFLAGS) all
endif
	@cd $(LIBPROCESS) && $(MAKE) $(AM_MAKEFLAGS) all
	@test $(top_srcdir) = $(top_builddir) ||                \
          test -d $(LEVELDB) ||                                 \
          cp -r $(top_srcdir)/$(LEVELDB) $(LEVELDB)
	@cd $(LEVELDB) && $(MAKE) $(AM_MAKEFLAGS) all
	@test $(top_srcdir) = $(top_builddir) ||                \
          test -d $(SETUPTOOLS) ||                              \
	  cp -r $(top_srcdir)/$(SETUPTOOLS) $(SETUPTOOLS)

clean-local:
	@echo "Cleaning third-party libraries"
	@cd $(PROTOBUF) && $(MAKE) $(AM_MAKEFLAGS) clean
	@cd $(GLOG) && $(MAKE) $(AM_MAKEFLAGS) clean
if WITH_INCLUDED_ZOOKEEPER
	@cd $(ZOOKEEPER) && $(MAKE) $(AM_MAKEFLAGS) clean
endif
	@cd $(LIBPROCESS) && $(MAKE) $(AM_MAKEFLAGS) clean
	@cd $(LEVELDB) && $(MAKE) $(AM_MAKEFLAGS) clean

if HAS_JAVA
maven-install:
	@cd src && $(MAKE) $(AM_MAKEFLAGS) maven-install

.PHONY: maven-install
endif
