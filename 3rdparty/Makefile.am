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

SUBDIRS = . stout libprocess

# We need to add '--srcdir=.' needed because 'make distcheck' adds
#  '--srcdir=...' when configuring.
CONFIGURE_ARGS = @CONFIGURE_ARGS@ --enable-shared=no --with-pic --srcdir=.

# The reason we use '/' prefix is to allow ourselves to install these packages
# in build/3rdparty directory as well. To do that, we can simply call
#   `make install DESTDIR=/path/to/builddir/ install`
# Similarly, when doing system-wide installation, we can do:
#   `make install DESTDIR=${MESOS-PREFIX}/lib/mesos/3rdparty install`
CONFIGURE_ARGS += --prefix=/

# Directory where bundled 3rdparty module dependencies are installed.
pkg3rdpartydir = $(pkglibdir)/3rdparty

include versions.am

BOOST = boost-$(BOOST_VERSION)
ELFIO = elfio-$(ELFIO_VERSION)
GLOG = glog-$(GLOG_VERSION)
GMOCK = gmock-$(GMOCK_VERSION)
GPERFTOOLS = gperftools-$(GPERFTOOLS_VERSION)
GTEST = $(GMOCK)/gtest
HTTP_PARSER = http-parser-$(HTTP_PARSER_VERSION)
LEVELDB = leveldb-$(LEVELDB_VERSION)
LIBEV = libev-$(LIBEV_VERSION)
NVML = nvml-$(NVML_VERSION)
PIP = pip-$(PIP_VERSION)
PROTOBUF = protobuf-$(PROTOBUF_VERSION)
PICOJSON = picojson-$(PICOJSON_VERSION)
SETUPTOOLS = setuptools-$(SETUPTOOLS_VERSION)
WHEEL = wheel-$(WHEEL_VERSION)
ZOOKEEPER = zookeeper-$(ZOOKEEPER_VERSION)

EXTRA_DIST =		\
  $(BOOST).tar.gz	\
  $(ELFIO).tar.gz	\
  $(GLOG).tar.gz	\
  $(GMOCK).tar.gz	\
  $(GPERFTOOLS).tar.gz	\
  $(HTTP_PARSER).tar.gz	\
  $(LEVELDB).tar.gz	\
  $(LIBEV).tar.gz	\
  $(NVML).tar.gz	\
  $(PIP).tar.gz		\
  $(PROTOBUF).tar.gz	\
  $(PICOJSON).tar.gz	\
  $(SETUPTOOLS).tar.gz	\
  $(WHEEL).tar.gz	\
  $(ZOOKEEPER).tar.gz

# We need to patch glog in order to deal with a compilation issue when
# compiling with clang (and C++11); see MESOS-860, MESOS-966.
EXTRA_DIST +=		\
  $(GLOG).patch

# We patch http-parser to avoid a name collision with our `flags` namespace.
EXTRA_DIST +=		\
  $(HTTP_PARSER).patch

# We need to patch leveldb to prevent it from linking to snappy and
# tcmalloc; see MESOS-1054, MESOS-1624.
EXTRA_DIST +=		\
  $(LEVELDB).patch

# We patch libev to keep it from reaping child processes.
EXTRA_DIST +=		\
  $(LIBEV).patch

# We include an upstream patch for protobuf in order to avoid
# signed/unsigned comparisons.
EXTRA_DIST +=		\
  $(PROTOBUF).patch

# We need to patch ZooKeeper in order to get 3.4.8 to compile on PPC LE.
EXTRA_DIST +=		\
  $(ZOOKEEPER).patch

CLEAN_EXTRACTED =	\
  $(BOOST)		\
  $(ELFIO)		\
  $(GLOG)		\
  $(GMOCK)		\
  $(GPERFTOOLS)		\
  $(HTTP_PARSER)	\
  $(LEVELDB)		\
  $(LIBEV)		\
  $(NVML)		\
  $(PIP)		\
  $(PICOJSON)		\
  $(PROTOBUF)		\
  $(SETUPTOOLS)		\
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
if WITH_BUNDLED_PIP
  ALL_LOCAL += $(PIP)-stamp
endif

if WITH_BUNDLED_SETUPTOOLS
  ALL_LOCAL += $(SETUPTOOLS)-stamp
endif

if WITH_BUNDLED_WHEEL
  ALL_LOCAL += $(WHEEL)-stamp
endif
endif

if WITH_BUNDLED_BOOST
  ALL_LOCAL += $(BOOST)-stamp
endif

if WITH_BUNDLED_ELFIO
# Stout depends on ELFIO. Install ELFIO into $PREFIX/include
# but don't add it to the source tarball.
elfiodir = $(includedir)/elfio

nodist_elfio_HEADERS =			\
  $(ELFIO)/elfio/elf_types.hpp		\
  $(ELFIO)/elfio/elfio.hpp		\
  $(ELFIO)/elfio/elfio_dump.hpp		\
  $(ELFIO)/elfio/elfio_dynamic.hpp	\
  $(ELFIO)/elfio/elfio_header.hpp	\
  $(ELFIO)/elfio/elfio_note.hpp		\
  $(ELFIO)/elfio/elfio_relocation.hpp	\
  $(ELFIO)/elfio/elfio_section.hpp	\
  $(ELFIO)/elfio/elfio_segment.hpp	\
  $(ELFIO)/elfio/elfio_strings.hpp	\
  $(ELFIO)/elfio/elfio_symbols.hpp	\
  $(ELFIO)/elfio/elfio_utils.hpp

$(ELFIO)/elfio/elf_types.hpp: $(ELFIO)-stamp
$(ELFIO)/elfio/elfio.hpp: $(ELFIO)-stamp
$(ELFIO)/elfio/elfio_dump.hpp: $(ELFIO)-stamp
$(ELFIO)/elfio/elfio_dynamic.hpp: $(ELFIO)-stamp
$(ELFIO)/elfio/elfio_header.hpp: $(ELFIO)-stamp
$(ELFIO)/elfio/elfio_note.hpp: $(ELFIO)-stamp
$(ELFIO)/elfio/elfio_relocation.hpp: $(ELFIO)-stamp
$(ELFIO)/elfio/elfio_section.hpp: $(ELFIO)-stamp
$(ELFIO)/elfio/elfio_segment.hpp: $(ELFIO)-stamp
$(ELFIO)/elfio/elfio_strings.hpp: $(ELFIO)-stamp
$(ELFIO)/elfio/elfio_symbols.hpp: $(ELFIO)-stamp
$(ELFIO)/elfio/elfio_utils.hpp: $(ELFIO)-stamp

ALL_LOCAL += $(ELFIO)-stamp
endif

if WITH_BUNDLED_GLOG
LIB_GLOG = $(GLOG)/libglog.la
GLOG_LDFLAGS = # Initialize to empty
if OS_FREEBSD
  GLOG_LDFLAGS += "LDFLAGS='-L/usr/local/lib -lexecinfo'"
endif

$(LIB_GLOG): $(GLOG)-build-stamp

# NOTE: We explicitly set GTEST_CONFIG=no so that we don't attempt to
# build glog against a system installed gtest (there's no reason to
# "test" glog and it's highly likely it will cause a compilation bug
# when building with clang).
# We explicitly build only libglog.la as it is the artifact we depend
# on. This e.g., avoids building glog-internal tests.
$(GLOG)-build-stamp: $(GLOG)-stamp
	cd $(GLOG) && ./configure GTEST_CONFIG=no $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS) $(GLOG_LDFLAGS) libglog.la
	touch $@

ALL_LOCAL += $(LIB_GLOG)
endif

if HAS_GPERFTOOLS
LIB_GPERFTOOLS = $(GPERFTOOLS)/libprofiler.la

$(LIB_GPERFTOOLS): $(GPERFTOOLS)-build-stamp

$(GPERFTOOLS)-build-stamp: $(GPERFTOOLS)-stamp
	cd $(GPERFTOOLS) && ./configure  $(CONFIGURE_ARGS) && \
	  $(MAKE) $(AM_MAKEFLAGS)
	touch $@

ALL_LOCAL += $(LIB_GPERFTOOLS)
endif


if WITH_BUNDLED_HTTP_PARSER
# Convenience library for nodejs/http-parser.
noinst_LTLIBRARIES = libry_http_parser.la
nodist_libry_http_parser_la_SOURCES = $(HTTP_PARSER)/http_parser.c
libry_http_parser_la_CPPFLAGS = -I$(HTTP_PARSER) -DHTTP_PARSER_STRICT=0

BUILT_SOURCES += $(nodist_libry_http_parser_la_SOURCES)

$(HTTP_PARSER)/http_parser.c: $(HTTP_PARSER)-stamp
$(HTTP_PARSER)-build-stamp: libry_http_parser.la
	touch $@
endif

if WITH_BUNDLED_LIBEV
$(LIBEV)/libev.la: $(LIBEV)-build-stamp
$(LIBEV)-build-stamp: $(LIBEV)-stamp
	cd $(LIBEV) && ./configure $(CONFIGURE_ARGS) && \
           $(MAKE) $(AM_MAKEFLAGS)
	touch $@
ALL_LOCAL += $(LIBEV)-build-stamp
endif

if WITH_BUNDLED_NVML
$(NVML)/nvidia/gdk/nvml.h: $(NVML)-stamp
ALL_LOCAL += $(NVML)-stamp
endif

if WITH_BUNDLED_PICOJSON
# Stout depends on <picojson.h>. Install picojson.h into $PREFIX/include
# but don't add it to the source tarball.
nodist_include_HEADERS = $(PICOJSON)/picojson.h
$(PICOJSON)/picojson.h: $(PICOJSON)-stamp
ALL_LOCAL += $(PICOJSON)-stamp
endif


if WITH_BUNDLED_PROTOBUF
$(PROTOBUF)/src/protoc $(PROTOBUF)/src/libprotobuf.la: $(PROTOBUF)-build-stamp

$(PROTOBUF)-build-stamp: $(PROTOBUF)-stamp
	cd $(PROTOBUF) && ./configure $(CONFIGURE_ARGS) && \
        $(MAKE) $(AM_MAKEFLAGS)
	touch $@
ALL_LOCAL += $(PROTOBUF)/src/libprotobuf.la
ALL_LOCAL += $(PROTOBUF)/src/protoc
endif

# Convenience library for gmock/gtest.
check_LTLIBRARIES = libgmock.la
nodist_libgmock_la_SOURCES = gmock_sources.cc

if WITH_BUNDLED_GMOCK
libgmock_la_CPPFLAGS =			\
  -I$(GTEST)/include -I$(GTEST) -I$(GTEST)/src	\
  -I$(GMOCK)/include -I$(GMOCK) -I$(GMOCK)/src

  gmock_sources.cc: $(GMOCK)-stamp

$(GMOCK)-build-stamp: libgmock.la
	touch $@
ALL_LOCAL += $(GMOCK)-build-stamp
endif

if LINKGTEST
libgmock_la_LIBADD = -lgtest
endif


# We list the sources in BUILT_SOURCES to make sure that the package
# gets unarchived first.
BUILT_SOURCES += $(nodist_libgmock_la_SOURCES)

if WITH_BUNDLED_LEVELDB
# TODO(charles): Figure out PIC options in our configure.ac or create
# a configure.ac for leveldb.
$(LEVELDB)/out-static/libleveldb.a: $(LEVELDB)-stamp
	cd $(LEVELDB) && \
	  $(MAKE) $(AM_MAKEFLAGS) CC="$(CC)" CXX="$(CXX)" OPT="$(CXXFLAGS) -fPIC"

ALL_LOCAL += $(LEVELDB)/out-static/libleveldb.a
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
if INSTALL_MODULE_DEPENDENCIES
	$(MAKE) $(AM_MAKEFLAGS) \
	  INSTALLDIR=$(abs_top_builddir)/3rdparty install-module-dependencies
endif

clean-local:
	rm -r -f $(CLEAN_EXTRACTED)
	rm -f *-stamp
if INSTALL_MODULE_DEPENDENCIES
	rm -rf bin include lib64 lib share
endif

tests: all $(check_LTLIBRARIES)
	@cd stout && $(MAKE) $(AM_MAKEFLAGS) tests
	@cd libprocess && $(MAKE) $(AM_MAKEFLAGS) tests

# Install 3rdparty module dependencies.
if INSTALL_MODULE_DEPENDENCIES
install-exec-local:
	$(MAKE) $(AM_MAKEFLAGS) \
	  INSTALLDIR=$(DESTDIR)$(pkg3rdpartydir) install-module-dependencies
endif

install-module-dependencies:
	$(install_sh) -d $(INSTALLDIR)/include
if WITH_BUNDLED_BOOST
	@test -d $(INSTALLDIR)/include/boost || \
	  rm -rf $(INSTALLDIR)/include/boost
	cp -fpR $(BOOST)/boost $(INSTALLDIR)/include/
endif
if WITH_BUNDLED_ELFIO
	@test -d $(INSTALLDIR)/include/elfio || \
	  rm -rf $(INSTALLDIR)/include/elfio
	cp -fpR $(ELFIO)/elfio $(INSTALLDIR)/include/
endif
if WITH_BUNDLED_GLOG
	cd $(GLOG) && \
	  $(MAKE) $(AM_MAKEFLAGS) DESTDIR=$(INSTALLDIR) install
endif
if WITH_BUNDLED_NVML
	@test -d $(INSTALLDIR)/include/nvidia || \
	  rm -rf $(INSTALLDIR)/include/nvidia
	cp -fpR $(NVML)/nvidia $(INSTALLDIR)/include/
endif
if WITH_BUNDLED_PICOJSON
	cd $(PICOJSON) && \
	  $(MAKE) $(AM_MAKEFLAGS) \
	    DESTDIR= includedir=$(INSTALLDIR)/include install
endif
if WITH_BUNDLED_PROTOBUF
	cd $(PROTOBUF) && \
	  $(MAKE) $(AM_MAKEFLAGS) DESTDIR=$(INSTALLDIR) install
endif
if WITH_BUNDLED_ZOOKEEPER
	cd $(ZOOKEEPER)/src/c && \
	  $(MAKE) $(AM_MAKEFLAGS) DESTDIR=$(INSTALLDIR) install
endif
