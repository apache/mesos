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

# This Makefile is for building third_party packages from
# tarballs. For autotools-based packages, we configure each of the
# packages to build static PIC binaries which we can safely link into
# a shared libmesos, and build it in-place without installing it (even
# if one runs 'make install' in this directory). Non-autotools based
# packages may be special cases; this Makefile is responsible for
# passing any special make or configure flags that might be required.

BUILT_SOURCES =

SUBDIRS = libprocess

# We need to add '--srcdir=.' needed because 'make distcheck' adds
#  '--srcdir=...' when configuring.
CONFIGURE_ARGS = @CONFIGURE_ARGS@ --enable-shared=no --with-pic --srcdir=.

include versions.am

BOOST = boost-$(BOOST_VERSION)
BOTO = boto-$(BOTO_VERSION)
DISTRIBUTE = distribute-$(DISTRIBUTE_VERSION)
GLOG = glog-$(GLOG_VERSION)
GMOCK = gmock-$(GMOCK_VERSION)
GPERFTOOLS = gperftools-$(GPERFTOOLS_VERSION)
GTEST = $(GMOCK)/gtest
LEVELDB = leveldb
PROTOBUF = protobuf-$(PROTOBUF_VERSION)
ZOOKEEPER = zookeeper-$(ZOOKEEPER_VERSION)


EXTRA_DIST = $(BOOST).tar.gz $(BOTO).zip $(DISTRIBUTE).tar.gz	\
	$(GLOG).tar.gz $(GMOCK).tar.gz $(GPERFTOOLS).tar.gz	\
	$(LEVELDB).tar.gz $(PROTOBUF).tar.gz $(ZOOKEEPER).tar.gz

CLEAN_EXTRACTED = $(BOOST) $(BOTO) $(DISTRIBUTE) $(GLOG) $(GMOCK)	\
	$(GPERFTOOLS) $(LEVELDB) $(PROTOBUF) $(ZOOKEEPER)


# Convenience library for gmock/gtest.
check_LTLIBRARIES = libgmock.la
nodist_libgmock_la_SOURCES = $(GTEST)/src/gtest-all.cc \
                             $(GMOCK)/src/gmock-all.cc
libgmock_la_CPPFLAGS = -I$(GTEST)/include -I$(GTEST) \
                       -I$(GMOCK)/include -I$(GMOCK)

# We list the gmock/gtest sources in BUILT_SOURCES to make sure that
# the gmock package gets unarchived first.
BUILT_SOURCES += $(nodist_libgmock_la_SOURCES)


# This is where the magic happens: we use stamp files as dependencies
# which cause the packages to get extracted as necessary.
%-stamp:: %.tar.gz
	gzip -d -c $^ | tar xf -
	touch $@


$(BOOST)/boost: $(BOOST)-stamp

if HAS_PYTHON
DISTRIBUTE_EGG = \
  $(DISTRIBUTE)/dist/$(DISTRIBUTE)$(PYTHON_EGG_PUREPY_POSTFIX).egg

$(DISTRIBUTE_EGG): $(DISTRIBUTE)-stamp
	cd $(DISTRIBUTE) && $(PYTHON) setup.py bdist_egg
endif

$(GLOG)/libglog.la: $(GLOG)-stamp
	cd $(GLOG) && ./configure $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)

$(GMOCK)/src/gmock-all.cc: $(GMOCK)-stamp

$(GPERFTOOLS)/libprofiler.la: $(GPERFTOOLS)-stamp
	cd $(GPERFTOOLS) && ./configure  $(CONFIGURE_ARGS) && \
	  $(MAKE) $(AM_MAKEFLAGS)

$(GTEST)/src/gtest-all.cc: $(GMOCK)-stamp

# TODO(charles): Figure out PIC options in our configure.ac or create
# a configure.ac for leveldb.
$(LEVELDB)/libleveldb.a: $(LEVELDB)-stamp
	cd $(LEVELDB) && \
          $(MAKE) $(AM_MAKEFLAGS) CC="$(CXX)" OPT="$(CXXFLAGS) -fPIC"

$(PROTOBUF)/src/protoc $(PROTOBUF)/src/libprotobuf.la: $(PROTOBUF)-build-stamp

$(PROTOBUF)-build-stamp: $(PROTOBUF)-stamp
	cd $(PROTOBUF) && ./configure $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)
	touch $@

$(ZOOKEEPER)/src/c/libzookeeper_mt.la: $(ZOOKEEPER)-stamp
	cd $(ZOOKEEPER)/src/c && ./configure $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)

THIRD_PARTY_LIBS = $(BOOST)-stamp $(GLOG)/libglog.la		\
	$(GPERFTOOLS)/libprofiler.la				\
	$(LEVELDB)/libleveldb.a $(PROTOBUF)/src/libprotobuf.la	\
	$(PROTOBUF)/src/protoc

if HAS_PYTHON
  THIRD_PARTY_LIBS += $(DISTRIBUTE_EGG)
endif

if WITH_INCLUDED_ZOOKEEPER
  THIRD_PARTY_LIBS += $(ZOOKEEPER)/src/c/libzookeeper_mt.la
else
  # Now matter what we need to extract ZooKeeper so that we can run
  # 'make check' (some of our tests need the ZooKeeper JAR).
  THIRD_PARTY_LIBS += $(ZOOKEEPER)-stamp
endif

all-local: $(THIRD_PARTY_LIBS)

clean-local:
	rm -r -f $(CLEAN_EXTRACTED)
	rm -f *-stamp
