# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
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

SUBDIRS = stout

BUILT_SOURCES = # Initialized to enable using +=.

# We need to add '--srcdir=.' needed because 'make distcheck' adds
#  '--srcdir=...' when configuring.
CONFIGURE_ARGS = @CONFIGURE_ARGS@ --enable-shared=no --with-pic --srcdir=.

include versions.am

STOUT = stout
BOOST = boost-$(BOOST_VERSION)
GLOG = glog-$(GLOG_VERSION)
GMOCK = gmock-$(GMOCK_VERSION)
GPERFTOOLS = gperftools-$(GPERFTOOLS_VERSION)
GTEST = $(GMOCK)/gtest
HTTP_PARSER = http-parser-$(HTTP_PARSER_VERSION)
LIBEV = libev-$(LIBEV_VERSION)
PROTOBUF = protobuf-$(PROTOBUF_VERSION)
PICOJSON = picojson-$(PICOJSON_VERSION)

EXTRA_DIST =			\
  $(BOOST).tar.gz		\
  $(GLOG).tar.gz		\
  $(GMOCK).tar.gz		\
  $(GPERFTOOLS).tar.gz		\
  $(LIBEV).tar.gz		\
  $(PROTOBUF).tar.gz		\
  $(HTTP_PARSER).tar.gz		\
  $(PICOJSON).tar.gz

# We need to patch glog in order to deal with a compilation issue when
# compiling with clang (and C++11); see MESOS-860, MESOS-966.
EXTRA_DIST +=			\
  $(GLOG).patch

# We patch libev to keep it from reaping child processes.
EXTRA_DIST +=			\
  $(LIBEV).patch

# We patch http-parser to avoid a name collision with our `flags` namespace.
EXTRA_DIST +=			\
  $(HTTP_PARSER).patch

CLEAN_EXTRACTED =	\
  $(BOOST)		\
  $(GLOG)		\
  $(GMOCK)		\
  $(GPERFTOOLS)		\
  $(LIBEV)		\
  $(PROTOBUF)		\
  $(HTTP_PARSER)	\
  $(PICOJSON)

ALL_LOCAL = # Initialize ALL_LOCAL so we can add to it.

# This is where the magic happens: we use stamp files as dependencies
# which cause the packages to get extracted as necessary. We also
# apply any patches as appropriate.
%-stamp: %.tar.gz
	gzip -d -c $^ | tar xf -
	test ! -e $(srcdir)/$*.patch || patch -d $* -p1 <$(srcdir)/$*.patch
	touch $@

if WITH_BUNDLED_HTTP_PARSER
  # Convenience library for nodejs/http-parser.
  noinst_LTLIBRARIES = libry_http_parser.la
  nodist_libry_http_parser_la_SOURCES = $(HTTP_PARSER)/http_parser.c
  libry_http_parser_la_CPPFLAGS = -I$(HTTP_PARSER) -DHTTP_PARSER_STRICT=0

  BUILT_SOURCES += $(nodist_libry_http_parser_la_SOURCES)

  $(HTTP_PARSER)/http_parser.c: $(HTTP_PARSER)-stamp
endif

if WITH_BUNDLED_PROTOBUF
  $(PROTOBUF)/src/protoc $(PROTOBUF)/src/libprotobuf.la: $(PROTOBUF)-build-stamp

  $(PROTOBUF)-build-stamp: $(PROTOBUF)-stamp
	cd $(PROTOBUF) && ./configure $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)
	touch $@

  LIBPROTOBUF = $(PROTOBUF)/src/libprotobuf.la

else
  LIBPROTOBUF = -lprotobuf
endif

# Convenience library for gmock/gtest.
check_LTLIBRARIES = libgmock.la
nodist_libgmock_la_SOURCES = gmock_sources.cc

if WITH_BUNDLED_GMOCK
  libgmock_la_CPPFLAGS =			\
  -I$(GTEST)/include -I$(GTEST) -I$(GTEST)/src	\
  -I$(GMOCK)/include -I$(GMOCK) -I$(GMOCK)/src

  gmock_sources.cc: $(GMOCK)-stamp
endif

if LINKGTEST
  libgmock_la_LIBADD = -lgtest
endif


# We list the sources in BUILT_SOURCES to make sure that the package
# gets unarchived first.
BUILT_SOURCES += $(nodist_libgmock_la_SOURCES)

GLOG_LDFLAGS = # Initialize to empty
if OS_FREEBSD
  GLOG_LDFLAGS += "LDFLAGS='-L/usr/local/lib -lexecinfo'"
endif

if WITH_BUNDLED_GLOG
# NOTE: We explicitly set GTEST_CONFIG=no so that we don't attempt to
# build glog against a system installed gtest (there's no reason to
# "test" glog and it's highly likely it will cause a compilation bug
# when building with clang).
$(GLOG)/libglog.la: $(GLOG)-stamp
	cd $(GLOG) && ./configure GTEST_CONFIG=no $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS) $(GLOG_LDFLAGS)
endif

if HAS_GPERFTOOLS
$(GPERFTOOLS)/libprofiler.la: $(GPERFTOOLS)-build-stamp

$(GPERFTOOLS)-build-stamp: $(GPERFTOOLS)-stamp
	cd $(GPERFTOOLS) && ./configure  $(CONFIGURE_ARGS) && \
	  $(MAKE) $(AM_MAKEFLAGS)
	touch $@
endif

if WITH_BUNDLED_LIBEV
$(LIBEV)/libev.la: $(LIBEV)-stamp
	cd $(LIBEV) && ./configure $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)
endif

# Tests for stout.
check_PROGRAMS = stout-tests

stout_tests_SOURCES =				\
  $(STOUT)/tests/adaptor_tests.cpp		\
  $(STOUT)/tests/base64_tests.cpp		\
  $(STOUT)/tests/bits_tests.cpp			\
  $(STOUT)/tests/bytes_tests.cpp		\
  $(STOUT)/tests/cache_tests.cpp		\
  $(STOUT)/tests/duration_tests.cpp		\
  $(STOUT)/tests/dynamiclibrary_tests.cpp	\
  $(STOUT)/tests/error_tests.cpp		\
  $(STOUT)/tests/flags_tests.cpp		\
  $(STOUT)/tests/gzip_tests.cpp			\
  $(STOUT)/tests/hashmap_tests.cpp		\
  $(STOUT)/tests/hashset_tests.cpp		\
  $(STOUT)/tests/interval_tests.cpp		\
  $(STOUT)/tests/ip_tests.cpp                   \
  $(STOUT)/tests/json_tests.cpp			\
  $(STOUT)/tests/jsonify_tests.cpp		\
  $(STOUT)/tests/linkedhashmap_tests.cpp	\
  $(STOUT)/tests/mac_tests.cpp                  \
  $(STOUT)/tests/main.cpp			\
  $(STOUT)/tests/multimap_tests.cpp		\
  $(STOUT)/tests/none_tests.cpp			\
  $(STOUT)/tests/numify_tests.cpp		\
  $(STOUT)/tests/option_tests.cpp		\
  $(STOUT)/tests/os_tests.cpp			\
  $(STOUT)/tests/os/env_tests.cpp		\
  $(STOUT)/tests/os/filesystem_tests.cpp	\
  $(STOUT)/tests/os/rmdir_tests.cpp		\
  $(STOUT)/tests/os/sendfile_tests.cpp		\
  $(STOUT)/tests/os/signals_tests.cpp		\
  $(STOUT)/tests/os/strerror_tests.cpp		\
  $(STOUT)/tests/path_tests.cpp			\
  $(STOUT)/tests/protobuf_tests.cpp		\
  $(STOUT)/tests/protobuf_tests.pb.cc		\
  $(STOUT)/tests/protobuf_tests.pb.h		\
  $(STOUT)/tests/protobuf_tests.proto		\
  $(STOUT)/tests/recordio_tests.cpp		\
  $(STOUT)/tests/result_tests.cpp		\
  $(STOUT)/tests/set_tests.cpp			\
  $(STOUT)/tests/some_tests.cpp			\
  $(STOUT)/tests/strings_tests.cpp		\
  $(STOUT)/tests/subcommand_tests.cpp		\
  $(STOUT)/tests/svn_tests.cpp			\
  $(STOUT)/tests/try_tests.cpp			\
  $(STOUT)/tests/uuid_tests.cpp			\
  $(STOUT)/tests/version_tests.cpp

if OS_LINUX
  stout_tests_SOURCES += $(STOUT)/tests/proc_tests.cpp
endif

stout_tests_CPPFLAGS =				\
  -I$(srcdir)/$(STOUT)/include			\
  -I$(PROTOBUF)/src				\
  $(AM_CPPFLAGS)

if WITH_BUNDLED_GMOCK
  stout_tests_CPPFLAGS += -I$(GTEST)/include
  stout_tests_CPPFLAGS += -I$(GMOCK)/include
endif

# We use "-isystem" instead of "-I" to add Boost to the include search
# path. This disables compiler warnings inside Boost headers since we
# can't easily fix them. See MESOS-3799.
if WITH_BUNDLED_BOOST
  stout_tests_CPPFLAGS += -isystem $(BOOST)
endif

if WITH_BUNDLED_PICOJSON
  stout_tests_CPPFLAGS += -I$(PICOJSON)

  # Enable the INT64 support for PicoJSON.
  stout_tests_CPPFLAGS += -DPICOJSON_USE_INT64
  stout_tests_CPPFLAGS += -D__STDC_FORMAT_MACROS

  # Stout depends on <picojson.h>. Install picojson.h into $PREFIX/include
  # but don't add it to the source tarball.
  nodist_include_HEADERS = $(PICOJSON)/picojson.h
  $(PICOJSON)/picojson.h: $(PICOJSON)-stamp
endif

if WITH_BUNDLED_GLOG
  stout_tests_CPPFLAGS += -I$(GLOG)/src
  LIBGLOG = $(GLOG)/libglog.la
else
  LIBGLOG = -lglog
endif

stout_tests_LDADD =			\
  libgmock.la				\
  $(LIBGLOG)				\
  $(LIBPROTOBUF)			\
  -lsvn_subr-1				\
  -lsvn_delta-1				\
  -lapr-1

# FreeBSD makes the dynamic linking interface available in libc
if !OS_FREEBSD
  stout_tests_LDADD += -ldl
endif

# We use a check-local target for now to avoid the parallel test
# runner that ships with newer versions of autotools.
# See the following discussion for the workaround:
# http://lists.gnu.org/archive/html/automake/2013-01/msg00051.html
check-local: tests
	./stout-tests

tests: all stout-tests

# Dependencies for all-local.
ALL_LOCAL += $(STOUT)/Makefile

if WITH_BUNDLED_PROTOBUF
  ALL_LOCAL += $(PROTOBUF)/src/libprotobuf.la
  ALL_LOCAL += $(PROTOBUF)/src/protoc
endif

if WITH_BUNDLED_LIBEV
  ALL_LOCAL += $(LIBEV)/libev.la
endif

if WITH_BUNDLED_BOOST
  ALL_LOCAL += $(BOOST)-stamp
endif

if WITH_BUNDLED_PICOJSON
  ALL_LOCAL += $(PICOJSON)-stamp
endif

if WITH_BUNDLED_GLOG
  ALL_LOCAL += $(LIBGLOG)
endif

if HAS_GPERFTOOLS
 ALL_LOCAL += $(GPERFTOOLS)/libprofiler.la
endif

all-local: $(ALL_LOCAL)

clean-local:
	rm -r -f $(CLEAN_EXTRACTED)
	rm -f *-stamp
