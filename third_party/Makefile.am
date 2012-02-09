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

# We need to add '--srcdir=.' needed because 'make distcheck' adds
#  '--srcdir=...' when configuring.
CONFIGURE_ARGS = @CONFIGURE_ARGS@ --enable-shared=no --with-pic --srcdir=.

GLOG_VERSION = 0.3.1
GMOCK_VERSION = 1.6.0
ZOOKEEPER_VERSION = 3.3.1
PROTOBUF_VERSION = 2.3.0
BOOST_VERSION = 1.37.0
DISTRIBUTE_VERSION = 0.6.19
BOTO_VERSION = 2.0b2

BOOST = boost-$(BOOST_VERSION)
BOTO = boto-$(BOTO_VERSION)
DISTRIBUTE = distribute-$(DISTRIBUTE_VERSION)
GLOG = glog-$(GLOG_VERSION)
GMOCK = gmock-$(GMOCK_VERSION)
GTEST = $(GMOCK)/gtest
LEVELDB = leveldb
PROTOBUF = protobuf-$(PROTOBUF_VERSION)
ZOOKEEPER = zookeeper-$(ZOOKEEPER_VERSION)
LIBPROCESS = libprocess

EXTRA_DIST = $(BOOST).tar.gz $(BOTO).tar.gz				\
	$(DISTRIBUTE)/distribute-$(DISTRIBUTE_VERSION)-py2.6.egg	\
	$(GLOG).tar.gz $(GMOCK).tar.gz $(LEVELDB).tar.gz		\
	$(PROTOBUF).tar.gz $(ZOOKEEPER).tar.gz

CLEAN_EXTRACTED = $(BOOST) $(BOTO) $(GLOG) $(GMOCK) $(LEVELDB)	\
	$(PROTOBUF) $(ZOOKEEPER)

check_LTLIBRARIES = libgmock.la
nodist_libgmock_la_SOURCES = $(GTEST)/src/gtest-all.cc \
                             $(GMOCK)/src/gmock-all.cc
libgmock_la_CPPFLAGS = -I$(GTEST)/include -I$(GTEST) \
                       -I$(GMOCK)/include -I$(GMOCK)

BUILT_SOURCES += $(nodist_libgmock_la_SOURCES)
%-stamp:: %.tar.gz
	gzip -d -c $^ | tar xf -
	touch $@

$(GTEST)/src/gtest-all.cc: $(GMOCK)-stamp
$(GMOCK)/src/gmock-all.cc: $(GMOCK)-stamp

$(GLOG)/libglog.la: $(GLOG)-stamp
	cd $(GLOG) && ./configure $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)

$(ZOOKEEPER)/src/c/libzookeeper_mt.la: $(ZOOKEEPER)-stamp
	cd $(ZOOKEEPER)/src/c && ./configure $(SUBCONFIG_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)

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

$(BOOST)/boost: $(BOOST)-stamp

THIRD_PARTY_LIBS = $(GLOG)/libglog.la $(PROTOBUF)/src/libprotobuf.la	\
	$(PROTOBUF)/src/protoc $(LEVELDB)/libleveldb.a $(DISTRIBUTE)	\
	$(BOTO)-stamp $(BOOST)-stamp

if WITH_INCLUDED_ZOOKEEPER
THIRD_PARTY_LIBS += $(ZOOKEEPER)/src/c/libzookeeper_mt.la
endif

all-local: $(THIRD_PARTY_LIBS)
	@cd $(LIBPROCESS) && $(MAKE) $(AM_MAKEFLAGS) all

clean-local:
	@cd $(LIBPROCESS) && $(MAKE) $(AM_MAKEFLAGS) clean
	rm -r -f $(CLEAN_EXTRACTED)
	rm -f *-stamp
