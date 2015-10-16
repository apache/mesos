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

# This Makefile is for building third-party packages from
# tarballs. For autotools-based packages, we configure each of the
# packages to build static PIC binaries which we can safely link into
# a shared libmesos, and build it in-place without installing it (even
# if one runs 'make install' in this directory). Non-autotools based
# packages may be special cases; this Makefile is responsible for
# passing any special make or configure flags that might be required.

BUILT_SOURCES = # Initialized to enable using +=.

SUBDIRS = libprocess

# We need to add '--srcdir=.' needed because 'make distcheck' adds
#  '--srcdir=...' when configuring.
CONFIGURE_ARGS = @CONFIGURE_ARGS@ --enable-shared=no --with-pic --srcdir=.

include versions.am

DISTRIBUTE = distribute-$(DISTRIBUTE_VERSION)
LEVELDB = leveldb
PIP = pip-$(PIP_VERSION)
WHEEL = wheel-$(WHEEL_VERSION)
ZOOKEEPER = zookeeper-$(ZOOKEEPER_VERSION)


EXTRA_DIST =		\
  $(DISTRIBUTE).tar.gz	\
  $(LEVELDB).tar.gz	\
  $(PIP).tar.gz		\
  $(WHEEL).tar.gz	\
  $(ZOOKEEPER).tar.gz

# We need to patch leveldb to prevent it from linking to snappy and
# tcmalloc; see MESOS-1054, MESOS-1624.
EXTRA_DIST +=		\
  $(LEVELDB).patch

# We need to patch ZooKeeper in order to get 3.4.5 to compile on
# OS X 10.10. See: MESOS-1797.
EXTRA_DIST +=		\
  $(ZOOKEEPER).patch

CLEAN_EXTRACTED =	\
  $(DISTRIBUTE)		\
  $(LEVELDB)		\
  $(PIP)		\
  $(WHEEL)		\
  $(ZOOKEEPER)

# This is where the magic happens: we use stamp files as dependencies
# which cause the packages to get extracted as necessary. We also
# apply any patches as appropriate.
%-stamp: %.tar.gz
	gzip -d -c $^ | tar xf -
	test ! -e $(srcdir)/$*.patch || patch -d $* -p1 <$(srcdir)/$*.patch
	touch $@

# Dependencies for all-local.
ALL_LOCAL = # Initialize ALL_LOCAL so we can add to it.

if HAS_PYTHON
if WITH_BUNDLED_DISTRIBUTE
  ALL_LOCAL += $(DISTRIBUTE)-stamp
endif

if WITH_BUNDLED_PIP
  ALL_LOCAL += $(PIP)-stamp
endif

if WITH_BUNDLED_WHEEL
  ALL_LOCAL += $(WHEEL)-stamp
endif
endif

if WITH_BUNDLED_LEVELDB

  # TODO(charles): Figure out PIC options in our configure.ac or create
  # a configure.ac for leveldb.
  $(LEVELDB)/libleveldb.a: $(LEVELDB)-stamp
	cd $(LEVELDB) && \
          $(MAKE) $(AM_MAKEFLAGS) CC="$(CC)" CXX="$(CXX)" OPT="$(CXXFLAGS) -fPIC"

  ALL_LOCAL += $(LEVELDB)/libleveldb.a
endif

if WITH_BUNDLED_ZOOKEEPER

  $(ZOOKEEPER)/src/c/libzookeeper_mt.la: $(ZOOKEEPER)-stamp
	cd $(ZOOKEEPER)/src/c && ./configure $(CONFIGURE_ARGS) && \
	  $(MAKE) $(AM_MAKEFLAGS)

  ALL_LOCAL += $(ZOOKEEPER)/src/c/libzookeeper_mt.la
else
  # No matter what, we need to extract ZooKeeper so that we can run
  # 'make check' (some of our tests need the ZooKeeper JAR).
  ALL_LOCAL += $(ZOOKEEPER)-stamp
endif

all-local: $(ALL_LOCAL)

clean-local:
	rm -r -f $(CLEAN_EXTRACTED)
	rm -f *-stamp

tests: all
	@cd libprocess && $(MAKE) $(AM_MAKEFLAGS) tests
