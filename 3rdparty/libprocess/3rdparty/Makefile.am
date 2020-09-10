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

BUILT_SOURCES = # Initialized to enable using +=.

# We need to add '--srcdir=.' needed because 'make distcheck' adds
#  '--srcdir=...' when configuring.
CONFIGURE_ARGS = @CONFIGURE_ARGS@ --enable-shared=no --with-pic --srcdir=.

# Set up building flags for non-bundled libraries.
SSL_INCLUDE_FLAGS = @SSL_CPPFLAGS@
SSL_LINKER_FLAGS = @SSL_LINKERFLAGS@
ZLIB_INCLUDE_FLAGS = @ZLIB_CPPFLAGS@
ZLIB_LINKER_FLAGS = @ZLIB_LINKERFLAGS@

# Directory which holds bundled dependencies
BUNDLED_DIR = $(top_srcdir)/..

include ../../versions.am

BOOST = boost-$(BOOST_VERSION)
CONCURRENTQUEUE = concurrentqueue-$(CONCURRENTQUEUE_VERSION)
GLOG = glog-$(GLOG_VERSION)
GOOGLETEST = googletest-release-$(GOOGLETEST_VERSION)
GMOCK = $(GOOGLETEST)/googlemock
GPERFTOOLS = gperftools-$(GPERFTOOLS_VERSION)
GRPC = grpc-$(GRPC_VERSION)
GTEST = $(GOOGLETEST)/googletest
HTTP_PARSER = http-parser-$(HTTP_PARSER_VERSION)
LIBEV = libev-$(LIBEV_VERSION)
PROTOBUF = protobuf-$(PROTOBUF_VERSION)
PICOJSON = picojson-$(PICOJSON_VERSION)
RAPIDJSON = rapidjson-$(RAPIDJSON_VERSION)

EXTRA_DIST =			\
  $(BOOST).tar.gz		\
  $(CONCURRENTQUEUE).tar.gz	\
  $(GLOG).tar.gz		\
  $(GOOGLETEST).tar.gz		\
  $(GPERFTOOLS).tar.gz		\
  $(GRPC).tar.gz		\
  $(HTTP_PARSER).tar.gz		\
  $(LIBEV).tar.gz		\
  $(PROTOBUF).tar.gz		\
  $(PICOJSON).tar.gz		\
  $(RAPIDJSON).tar.gz

# We need to patch glog in order to deal with a compilation issue when
# compiling with clang (and C++11); see MESOS-860, MESOS-966.
EXTRA_DIST +=		\
  $(GLOG).patch

# We patch http-parser to avoid a name collision with our `flags` namespace.
EXTRA_DIST +=		\
  $(HTTP_PARSER).patch

# We patch libev to keep it from reaping child processes.
EXTRA_DIST +=		\
  $(LIBEV).patch

# We include an upstream patch for protobuf in order to avoid
# signed/unsigned comparisons.
EXTRA_DIST +=		\
  $(PROTOBUF).patch

CLEAN_EXTRACTED =		\
  $(BOOST)			\
  $(CONCURRENTQUEUE)		\
  $(GLOG)			\
  $(GRPC)			\
  $(GOOGLETEST)			\
  $(GPERFTOOLS)			\
  $(HTTP_PARSER)		\
  $(LIBEV)			\
  $(PICOJSON)			\
  $(PROTOBUF)			\
  $(RAPIDJSON)

# This is where the magic happens: we use stamp files as dependencies
# which cause the packages to get extracted as necessary. We also
# apply any patches as appropriate.
%-stamp: $(BUNDLED_DIR)/%.tar.gz
	gzip -d -c $^ | tar xf - --no-same-owner
	test ! -e $(top_srcdir)/../$*.patch || patch -d $* -p1 <$(top_srcdir)/../$*.patch
	touch $@

# Dependencies for all-local.
ALL_LOCAL = # Initialize ALL_LOCAL so we can add to it.

if WITH_BUNDLED_BOOST
  ALL_LOCAL += $(BOOST)-stamp
endif

if WITH_BUNDLED_CONCURRENTQUEUE
  ALL_LOCAL += $(CONCURRENTQUEUE)-stamp
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

ALL_LOCAL += $(HTTP_PARSER)-build-stamp
endif


if WITH_BUNDLED_LIBEV
$(LIBEV)/libev.la: $(LIBEV)-build-stamp
$(LIBEV)-build-stamp: $(LIBEV)-stamp
	cd $(LIBEV) && ./configure $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)
	touch $@
ALL_LOCAL += $(LIBEV)-build-stamp
endif

if WITH_BUNDLED_PICOJSON
ALL_LOCAL += $(PICOJSON)-stamp
endif

if WITH_BUNDLED_RAPIDJSON
ALL_LOCAL += $(RAPIDJSON)-stamp
endif

if WITH_BUNDLED_PROTOBUF
$(PROTOBUF)/src/protoc $(PROTOBUF)/src/libprotobuf.la: $(PROTOBUF)-build-stamp

# NOTE: The `--with-zlib` flag works differently between libprocess and
# Protobuf. Protobuf uses `--with-zlib` as an on/off switch for data
# compression instead of a path specifier for ZLib, so we need to set up
# the `CPPFLAGS` and `LDFLAGS` if `--with-zlib=DIR` is given.
$(PROTOBUF)-build-stamp: $(PROTOBUF)-stamp
	cd $(PROTOBUF) &&				\
	  ./configure $(CONFIGURE_ARGS)			\
	              CPPFLAGS="$(ZLIB_INCLUDE_FLAGS)"	\
	              LDFLAGS="$(ZLIB_LINKER_FLAGS)" &&	\
	  $(MAKE) $(AM_MAKEFLAGS)
	touch $@
ALL_LOCAL += $(PROTOBUF)/src/libprotobuf.la
ALL_LOCAL += $(PROTOBUF)/src/protoc

PROTOBUF_INCLUDE_FLAGS = -I$(abs_builddir)/$(PROTOBUF)/src
PROTOBUF_LINKER_FLAGS = -L$(abs_builddir)/$(PROTOBUF)/src/.libs
PROTOC = $(abs_builddir)/$(PROTOBUF)/src/protoc
else
$(PROTOBUF)-build-stamp:
	touch $@

PROTOBUF_INCLUDE_FLAGS = @PROTOBUF_CPPFLAGS@
PROTOBUF_LINKER_FLAGS = @PROTOBUF_LINKERFLAGS@
PROTOC = @PROTOCOMPILER@
endif

if !ENABLE_SSL
GRPC_VARIANT = _unsecure
endif

if WITH_BUNDLED_GRPC
LIB_GRPC = $(GRPC)/libs/opt/libgrpc++$(GRPC_VARIANT).a	\
           $(GRPC)/libs/opt/libgrpc$(GRPC_VARIANT).a	\
           $(GRPC)/libs/opt/libgpr.a

$(LIB_GRPC): $(GRPC)-build-stamp

# NOTE: We pass compiler flags through environment variables rather than the
# command line because gRPC uses target-specific assignments to append compiler
# flags, which will be overwritten by the command line. See:
# https://www.gnu.org/software/make/manual/html_node/Target_002dspecific.html
#
# NOTE: We ignore `array-bounds` errors generated by the GLIBC's `__strcmp_cg`
# macro used in gRPC's c-ares library when compiled with Clang.
$(GRPC)-build-stamp: $(GRPC)-stamp			\
                     $(PROTOBUF)-build-stamp
	cd $(GRPC) &&					\
	  CPPFLAGS="$(PROTOBUF_INCLUDE_FLAGS)		\
	            $(SSL_INCLUDE_FLAGS)		\
	            $(ZLIB_INCLUDE_FLAGS)		\
	            -Wno-array-bounds			\
	            $(CPPFLAGS)"			\
	  CFLAGS="$(CFLAGS)"				\
	  CXXFLAGS="$(CXXFLAGS)"			\
	  $(MAKE) $(AM_MAKEFLAGS)			\
	    $(LIB_GRPC:%=$(abs_builddir)/%)		\
	    CC="$(CC)"					\
	    CXX="$(CXX)"				\
	    LD="$(CC)"					\
	    LDXX="$(CXX)"				\
	    LDFLAGS="$(PROTOBUF_LINKER_FLAGS)		\
	             $(SSL_LINKER_FLAGS)		\
	             $(ZLIB_LINKER_FLAGS)		\
	             $(LDFLAGS)"			\
	    LDLIBS="$(LDLIBS)"				\
	    HAS_PKG_CONFIG=false			\
	    NO_PROTOC=false				\
	    PROTOC="$(PROTOC)"
	touch $@

ALL_LOCAL += $(LIB_GRPC)
else
$(GRPC)-build-stamp:
	touch $@
endif

# Convenience library for gmock/gtest.
check_LTLIBRARIES = libgmock.la
nodist_libgmock_la_SOURCES = gmock_sources.cc

if WITH_BUNDLED_GMOCK
libgmock_la_CPPFLAGS =			\
  -I$(GTEST)/include -I$(GTEST) -I$(GTEST)/src	\
  -I$(GMOCK)/include -I$(GMOCK) -I$(GMOCK)/src

  gmock_sources.cc: $(GOOGLETEST)-stamp

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

all-local: $(ALL_LOCAL)

clean-local:
	rm -r -f $(CLEAN_EXTRACTED)
	rm -f *-stamp
