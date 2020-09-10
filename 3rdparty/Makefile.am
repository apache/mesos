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

# Set up building flags for non-bundled libraries.
SSL_INCLUDE_FLAGS = @SSL_CPPFLAGS@
SSL_LINKER_FLAGS = @SSL_LINKERFLAGS@
ZLIB_INCLUDE_FLAGS = @ZLIB_CPPFLAGS@
ZLIB_LINKER_FLAGS = @ZLIB_LINKERFLAGS@

# Directory where bundled 3rdparty module dependencies are installed.
pkg3rdpartydir = $(pkglibdir)/3rdparty

include versions.am

BOOST = boost-$(BOOST_VERSION)
CONCURRENTQUEUE = concurrentqueue-$(CONCURRENTQUEUE_VERSION)
CSI_V0 = csi-$(CSI_V0_VERSION)
CSI_V1 = csi-$(CSI_V1_VERSION)
ELFIO = elfio-$(ELFIO_VERSION)
GLOG = glog-$(GLOG_VERSION)
GMOCK = $(GOOGLETEST)/googlemock
GOOGLETEST = googletest-release-$(GOOGLETEST_VERSION)
GPERFTOOLS = gperftools-$(GPERFTOOLS_VERSION)
GRPC = grpc-$(GRPC_VERSION)
GTEST = $(GOOGLETEST)/googletest
HTTP_PARSER = http-parser-$(HTTP_PARSER_VERSION)
JEMALLOC = jemalloc-$(JEMALLOC_VERSION)
LEVELDB = leveldb-$(LEVELDB_VERSION)
LIBARCHIVE = libarchive-$(LIBARCHIVE_VERSION)
LIBEV = libev-$(LIBEV_VERSION)
LIBEVENT = libevent-$(LIBEVENT_VERSION)
LIBSECCOMP = libseccomp-$(LIBSECCOMP_VERSION)
NVML = nvml-$(NVML_VERSION)
PIP = pip-$(PIP_VERSION)
PICOJSON = picojson-$(PICOJSON_VERSION)
PROTOBUF = protobuf-$(PROTOBUF_VERSION)
RAPIDJSON = rapidjson-$(RAPIDJSON_VERSION)
RE2 = re2-$(RE2_VERSION)
SETUPTOOLS = setuptools-$(SETUPTOOLS_VERSION)
WHEEL = wheel-$(WHEEL_VERSION)
ZOOKEEPER = zookeeper-$(ZOOKEEPER_VERSION)

EXTRA_DIST =			\
  $(BOOST).tar.gz		\
  $(CONCURRENTQUEUE).tar.gz	\
  $(CSI_V0).tar.gz		\
  $(CSI_V1).tar.gz		\
  $(ELFIO).tar.gz		\
  $(GLOG).tar.gz		\
  $(GOOGLETEST).tar.gz		\
  $(GPERFTOOLS).tar.gz		\
  $(GRPC).tar.gz		\
  $(HTTP_PARSER).tar.gz		\
  $(JEMALLOC).tar.gz		\
  $(LEVELDB).tar.gz		\
  $(LIBARCHIVE).tar.gz		\
  $(LIBEV).tar.gz		\
  $(LIBEVENT).tar.gz		\
  $(LIBSECCOMP).tar.gz		\
  $(NVML).tar.gz		\
  $(PIP).tar.gz			\
  $(PROTOBUF).tar.gz		\
  $(PICOJSON).tar.gz		\
  $(RAPIDJSON).tar.gz		\
  $(RE2).tar.gz			\
  $(SETUPTOOLS).tar.gz		\
  $(WHEEL).tar.gz		\
  $(ZOOKEEPER).tar.gz

# We need to patch glog in order to deal with a compilation issue when
# compiling with clang (and C++11); see MESOS-860, MESOS-966.
EXTRA_DIST +=			\
  $(GLOG).patch

# We need to patch gRPC for non-SSL build; see MEOS-9149.
EXTRA_DIST +=			\
  $(GRPC).patch

# We need to patch leveldb to prevent it from linking to snappy and
# tcmalloc; see MESOS-1054, MESOS-1624.
EXTRA_DIST +=			\
  $(LEVELDB).patch

# We need to patch libevent 2.0.22 for added compatibilty towards openssl 1.1.x.
EXTRA_DIST +=			\
  $(LIBEVENT).patch

# We need to patch PicoJSON for limiting the nesting level of JSON objects.
EXTRA_DIST +=			\
  $(PICOJSON).patch

# We need to patch ZooKeeper in order to get 3.4.8 to compile on PPC LE.
EXTRA_DIST +=			\
  $(ZOOKEEPER).patch

# We need to patch RapidJSON to make it write infinite floating point numbers
# as "Infinity" and "-Infinity" (i.e., with double quotes) rather than Infinity
# and -Infinity. This is to ensure the strings converted from JSON objects
# conform to the rule defined by Protobuf:
#   https://developers.google.com/protocol-buffers/docs/proto3#json
EXTRA_DIST +=			\
  $(RAPIDJSON).patch

# We need the following patches for CMake and/or Windows builds.
EXTRA_DIST +=			\
  $(BOOST).patch		\
  $(GOOGLETEST).patch		\
  $(LIBARCHIVE).patch		\
  $(PROTOBUF).patch		\
  bzip2-1.0.6.patch		\
  cyrus-sasl-2.1.27rc3.patch	\
  xz-5.2.3.patch

CLEAN_EXTRACTED =		\
  $(BOOST)			\
  $(CONCURRENTQUEUE)		\
  $(CSI_V0)			\
  $(CSI_V1)			\
  $(ELFIO)			\
  $(GLOG)			\
  $(GOOGLETEST)			\
  $(GPERFTOOLS)			\
  $(GRPC)			\
  $(HTTP_PARSER)		\
  $(JEMALLOC)			\
  $(LEVELDB)			\
  $(LIBARCHIVE)			\
  $(LIBEV)			\
  $(LIBEVENT)			\
  $(LIBSECCOMP)			\
  $(NVML)			\
  $(PIP)			\
  $(PICOJSON)			\
  $(PROTOBUF)			\
  $(RAPIDJSON)			\
  $(RE2)			\
  $(SETUPTOOLS)			\
  $(WHEEL)			\
  $(ZOOKEEPER)

# This is where the magic happens: we use stamp files as dependencies
# which cause the packages to get extracted as necessary. We also
# apply any patches as appropriate.
%-stamp: %.tar.gz
	gzip -d -c $^ | tar xf - --no-same-owner
	test ! -e $(srcdir)/$*.patch || patch -d $* -p1 <$(srcdir)/$*.patch
	touch $@

# NOTE: The upstream CSI release tarball is packed with top-level directory
# `spec-<X.Y.Z>`. To avoid conflicts and be consistent with where other packages
# are extracted, we extract the tarball in directory `csi-<X.Y.Z>`.
#
# NOTE: Since GNU make 3.81 does not support shortest-stem pattern matching, we
# explicitly specify a static pattern rule here.
CSI_STAMPS = $(CSI_V0)-stamp $(CSI_V1)-stamp
$(CSI_STAMPS): csi-%-stamp: csi-%.tar.gz
	$(MKDIR_P) csi-$*
	gzip -d -c $^ | (cd csi-$* && tar xf - --no-same-owner)
	test ! -e $(srcdir)/csi-$*.patch || \
	  patch -d csi-$*/spec-$* -p1 <$(srcdir)/csi-$*.patch
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

if WITH_BUNDLED_CONCURRENTQUEUE
# Libprocess depends on <concurrentqueue.h>. Install concurrentqeueue.h
# into $PREFIX/include but don't add it to the source tarball.
nodist_include_HEADERS = $(CONCURRENTQUEUE)/concurrentqueue.h
$(CONCURRENTQUEUE)/concurrentqueue.h: $(CONCURRENTQUEUE)-stamp
ALL_LOCAL += $(CONCURRENTQUEUE)-stamp
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
#
# NOTE:
# On some platforms patch fails to remove test-driver (it is a broken symlink),
# that's why we need to remove it here.
# Also, autogen.sh might be CREATED by the patch and might need its mode fixed.
$(GLOG)-build-stamp: $(GLOG)-stamp
	cd $(GLOG) &&\
	  ./autogen.sh && \
	  ./configure GTEST_CONFIG=no $(CONFIGURE_ARGS) && \
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


if WITH_BUNDLED_JEMALLOC
JEMALLOC_CONFIGURE_ARGS = \
    --enable-stats --enable-prof --with-malloc-conf="prof:true,prof_active:false"

LIB_JEMALLOC = $(JEMALLOC)/libjemalloc.la

$(LIB_JEMALLOC): $(JEMALLOC)-build-stamp

$(JEMALLOC)-build-stamp: $(JEMALLOC)-stamp
	cd $(JEMALLOC) && ./configure $(JEMALLOC_CONFIGURE_ARGS) $(CONFIGURE_ARGS)
	cd $(JEMALLOC) && $(MAKE) $(AM_MAKEFLAGS)
	touch $@

ALL_LOCAL += $(LIB_JEMALLOC)
endif

if WITH_BUNDLED_LIBARCHIVE
LIB_LIBARCHIVE = $(LIBARCHIVE)/.libs/libarchive.la

$(LIB_LIBARCHIVE): $(LIBARCHIVE)-build-stamp

$(LIBARCHIVE)-build-stamp: $(LIBARCHIVE)-stamp
	cd $(LIBARCHIVE) &&			\
	  ./configure $(CONFIGURE_ARGS)		\
	      --without-bz2lib			\
	      --without-cng			\
	      --without-expat			\
	      --without-iconv			\
	      --without-libiconv-prefix		\
	      --without-lz4			\
	      --without-lzma			\
	      --without-nettle			\
	      --without-openssl			\
	      --without-xml2
	cd $(LIBARCHIVE) && $(MAKE) $(AM_MAKEFLAGS)
	touch $@

ALL_LOCAL += $(LIB_LIBARCHIVE)
endif

if WITH_BUNDLED_LIBEV
$(LIBEV)/libev.la: $(LIBEV)-build-stamp
$(LIBEV)-build-stamp: $(LIBEV)-stamp
	cd $(LIBEV) && ./configure $(CONFIGURE_ARGS) && \
           $(MAKE) $(AM_MAKEFLAGS)
	touch $@
ALL_LOCAL += $(LIBEV)-build-stamp
endif

if WITH_BUNDLED_LIBEVENT
$(LIBEVENT)/libevent.la: $(LIBEVENT)-build-stamp
$(LIBEVENT)-build-stamp: $(LIBEVENT)-stamp
	cd $(LIBEVENT) &&			\
	CPPFLAGS=$(SSL_INCLUDE_FLAGS)		\
	LDFLAGS=$(SSL_LINKER_FLAGS)		\
	./configure $(CONFIGURE_ARGS)		\
		--disable-dependency-tracking	\
		--disable-debug-mode		\
		--disable-samples &&		\
	$(MAKE) $(AM_MAKEFLAGS)
	touch $@
ALL_LOCAL += $(LIBEVENT)-build-stamp
endif

if WITH_BUNDLED_LIBSECCOMP
LIBSECCOMP_CONFIGURE_ARGS = --disable-shared

LIB_LIBSECCOMP = $(LIBSECCOMP)/src/libseccomp.la

$(LIB_LIBSECCOMP): $(LIBSECCOMP)-build-stamp

$(LIBSECCOMP)-build-stamp: $(LIBSECCOMP)-stamp
	cd $(LIBSECCOMP) &&						\
	  ./configure $(LIBSECCOMP_CONFIGURE_ARGS) $(CONFIGURE_ARGS) &&	\
	  $(MAKE) $(AM_MAKEFLAGS)
	touch $@

ALL_LOCAL += $(LIB_LIBSECCOMP)
endif

if ENABLE_NVML
if WITH_BUNDLED_NVML
$(NVML)/nvidia/gdk/nvml.h: $(NVML)-stamp
ALL_LOCAL += $(NVML)-stamp
endif
endif

if WITH_BUNDLED_PICOJSON
# Stout depends on <picojson.h>. Install picojson.h into $PREFIX/include
# but don't add it to the source tarball.
nodist_include_HEADERS = $(PICOJSON)/picojson.h
$(PICOJSON)/picojson.h: $(PICOJSON)-stamp
ALL_LOCAL += $(PICOJSON)-stamp
endif

if WITH_BUNDLED_RAPIDJSON
# Stout depends on rapidjson. Install rapidjson into $PREFIX/include
# but don't add it to the source tarball.
#
# We need to use a variable per sub-directory in order to preserve
# the subdirectory structure:
#   https://www.gnu.org/software/automake/manual/html_node/Alternative.html
rapidjsondir = $(includedir)/rapidjson
rapidjsonerrordir = $(includedir)/rapidjson/error
rapidjsoninternaldir = $(includedir)/rapidjson/internal
rapidjsonmsinttypesdir = $(includedir)/rapidjson/msinttypes

nodist_rapidjson_HEADERS =				\
  $(RAPIDJSON)/include/rapidjson/allocators.h		\
  $(RAPIDJSON)/include/rapidjson/document.h		\
  $(RAPIDJSON)/include/rapidjson/encodedstream.h	\
  $(RAPIDJSON)/include/rapidjson/encodings.h		\
  $(RAPIDJSON)/include/rapidjson/filereadstream.h	\
  $(RAPIDJSON)/include/rapidjson/filewritestream.h	\
  $(RAPIDJSON)/include/rapidjson/fwd.h			\
  $(RAPIDJSON)/include/rapidjson/istreamwrapper.h	\
  $(RAPIDJSON)/include/rapidjson/memorybuffer.h		\
  $(RAPIDJSON)/include/rapidjson/memorystream.h		\
  $(RAPIDJSON)/include/rapidjson/ostreamwrapper.h	\
  $(RAPIDJSON)/include/rapidjson/pointer.h		\
  $(RAPIDJSON)/include/rapidjson/prettywriter.h		\
  $(RAPIDJSON)/include/rapidjson/rapidjson.h		\
  $(RAPIDJSON)/include/rapidjson/reader.h		\
  $(RAPIDJSON)/include/rapidjson/schema.h		\
  $(RAPIDJSON)/include/rapidjson/stream.h		\
  $(RAPIDJSON)/include/rapidjson/stringbuffer.h		\
  $(RAPIDJSON)/include/rapidjson/writer.h

nodist_rapidjsonerror_HEADERS =				\
  $(RAPIDJSON)/include/rapidjson/error/en.h		\
  $(RAPIDJSON)/include/rapidjson/error/error.h

nodist_rapidjsoninternal_HEADERS =			\
  $(RAPIDJSON)/include/rapidjson/internal/biginteger.h	\
  $(RAPIDJSON)/include/rapidjson/internal/diyfp.h	\
  $(RAPIDJSON)/include/rapidjson/internal/dtoa.h	\
  $(RAPIDJSON)/include/rapidjson/internal/ieee754.h	\
  $(RAPIDJSON)/include/rapidjson/internal/itoa.h	\
  $(RAPIDJSON)/include/rapidjson/internal/meta.h	\
  $(RAPIDJSON)/include/rapidjson/internal/pow10.h	\
  $(RAPIDJSON)/include/rapidjson/internal/regex.h	\
  $(RAPIDJSON)/include/rapidjson/internal/stack.h	\
  $(RAPIDJSON)/include/rapidjson/internal/strfunc.h	\
  $(RAPIDJSON)/include/rapidjson/internal/strtod.h	\
  $(RAPIDJSON)/include/rapidjson/internal/swap.h

nodist_rapidjsonmsinttypes_HEADERS =			\
  $(RAPIDJSON)/include/rapidjson/msinttypes/inttypes.h	\
  $(RAPIDJSON)/include/rapidjson/msinttypes/stdint.h

$(nodist_rapidjson_HEADERS): $(RAPIDJSON)-stamp
$(nodist_rapidjsonerror_HEADERS): $(RAPIDJSON)-stamp
$(nodist_rapidjsoninternal_HEADERS): $(RAPIDJSON)-stamp
$(nodist_rapidjsonmsinttypes_HEADERS): $(RAPIDJSON)-stamp

ALL_LOCAL += $(RAPIDJSON)-stamp
endif

if WITH_BUNDLED_PROTOBUF
$(PROTOBUF)/src/protoc $(PROTOBUF)/src/libprotobuf.la: $(PROTOBUF)-build-stamp

# NOTE: The `--with-zlib` flag works differently between Mesos and
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
# NOTE: We use EXTRA_CPPFLAGS to pass `-Wno-error`, because that one needs to be
# appended and not prepended to the constructed flags.
#
# NOTE: We compile gRPC with its embedded c-ares library to avoid a link error
# because Mesos is not aware of the library; see MESOS-9505. If bundle c-ares
# to resolve MESOS-9338 in the future, we should remove `HAS_SYSTEM_CARES` below
# and set up proper include and linker flags instead.
$(GRPC)-build-stamp: $(GRPC)-stamp			\
                     $(PROTOBUF)-build-stamp
	cd $(GRPC) &&					\
	  CPPFLAGS="$(PROTOBUF_INCLUDE_FLAGS)		\
	            $(SSL_INCLUDE_FLAGS)		\
	            $(ZLIB_INCLUDE_FLAGS)		\
	            $(CPPFLAGS)"			\
	  CFLAGS="$(CFLAGS)"				\
	  CXXFLAGS="$(CXXFLAGS)"			\
	  $(MAKE) $(AM_MAKEFLAGS)			\
	    $(LIB_GRPC:%=$(realpath $(abs_builddir))/%)	\
	    CC="$(CC)"					\
	    CXX="$(CXX)"				\
	    EXTRA_CPPFLAGS=-Wno-error			\
	    LD="$(CC)"					\
	    LDXX="$(CXX)"				\
	    LDFLAGS="$(PROTOBUF_LINKER_FLAGS)		\
	             $(SSL_LINKER_FLAGS)		\
	             $(ZLIB_LINKER_FLAGS)		\
	             $(LDFLAGS)"			\
	    LDLIBS="$(LDLIBS)"				\
	    HAS_PKG_CONFIG=false			\
	    HAS_SYSTEM_CARES=false			\
	    HAS_PROTOC=true				\
	    HAS_VALID_PROTOC=true			\
	    PROTOC="$(PROTOC)"
	touch $@

ALL_LOCAL += $(LIB_GRPC)
else
$(GRPC)-build-stamp:
	touch $@
endif

# NOTE: To support multiple CSI versions, we move the CSI proto files to
# version-qualified paths so `protoc` can find them.
$(CSI_V0)-build-stamp: $(CSI_V0)-stamp
	$(MKDIR_P) $(CSI_V0)/csi/v0
	mv $(CSI_V0)/spec-$(CSI_V0_VERSION)/csi.proto $(CSI_V0)/csi/v0
	touch $@

$(CSI_V1)-build-stamp: $(CSI_V1)-stamp
	$(MKDIR_P) $(CSI_V1)/csi/v1
	mv $(CSI_V1)/spec-$(CSI_V1_VERSION)/csi.proto $(CSI_V1)/csi/v1
	touch $@

# Mesos depends on CSI. Install the CSI proto file into $PREFIX/include but
# don't add it to the source tarball.
csi_v0dir = $(includedir)/csi/v0
nodist_csi_v0_HEADERS = $(CSI_V0)/csi/v0/csi.proto
$(nodist_csi_v0_HEADERS): $(CSI_V0)-build-stamp

csi_v1dir = $(includedir)/csi/v1
nodist_csi_v1_HEADERS = $(CSI_V1)/csi/v1/csi.proto
$(nodist_csi_v1_HEADERS): $(CSI_V1)-build-stamp

ALL_LOCAL += $(CSI_V0)-build-stamp
ALL_LOCAL += $(CSI_V1)-build-stamp

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

if WITH_BUNDLED_LEVELDB
# TODO(charles): Figure out PIC options in our configure.ac or create
# a configure.ac for leveldb.
$(LEVELDB)/out-static/libleveldb.a: $(LEVELDB)-stamp
	cd $(LEVELDB) && \
	  $(MAKE) $(AM_MAKEFLAGS) CC="$(CC)" CXX="$(CXX)" OPT="$(CXXFLAGS) -fPIC"

ALL_LOCAL += $(LEVELDB)/out-static/libleveldb.a
endif

if WITH_BUNDLED_RE2
LIB_RE2 = $(RE2)/obj/libre2.a

$(LIB_RE2): $(RE2)-build-stamp

$(RE2)-build-stamp: $(RE2)-stamp
	cd $(RE2) && $(MAKE) $(AM_MAKEFLAGS) CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS) -fPIC"
	touch $@

ALL_LOCAL += $(LIB_RE2)
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
if WITH_BUNDLED_CONCURRENTQUEUE
	@test -d $(INSTALLDIR)/include/concurrentqueue || \
	  rm -rf $(INSTALLDIR)/include/concurrentqueue
	cp -fpR $(CONCURRENTQUEUE)/concurrentqueue.h $(INSTALLDIR)/include/
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
if WITH_BUNDLED_RAPIDJSON
# TODO(bmahler): Rapidjson has a cmake build, can we just cmake install
# instead of copying?
	@test -d $(INSTALLDIR)/include/rapidjson || \
	  rm -rf $(INSTALLDIR)/include/rapidjson
	cp -fpR $(RAPIDJSON)/include/rapidjson $(INSTALLDIR)/include/
endif
if WITH_BUNDLED_RE2
	cd $(RE2) && \
	  $(MAKE) $(AM_MAKEFLAGS) DESTDIR=$(INSTALLDIR) install
endif
if WITH_BUNDLED_ZOOKEEPER
	cd $(ZOOKEEPER)/src/c && \
	  $(MAKE) $(AM_MAKEFLAGS) DESTDIR=$(INSTALLDIR) install
endif
