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

# Makefile for stout.

# Enable common (and some language specific) warnings.
AM_CXXFLAGS = -Wall
# Warn when a comparison is made between signed and unsigned values.
AM_CXXFLAGS += -Wsign-compare
# Warn about use of format functions that can produce security issues.
AM_CXXFLAGS += -Wformat-security

# We will also have much more hardened/secured binaries and libraries.
if ENABLE_HARDENING
# Protect many of the functions with stack guards
# (either -fstack-protector-strong or -fstack-protector depending on compiler support).
AM_CXXFLAGS += @STACK_PROTECTOR@
# Produce position independent code when appropriate.
AM_CXXFLAGS += -fPIC -fPIE
endif

AUTOMAKE_OPTIONS = foreign

if STANDALONE_STOUT
MAYBE_3RDPARTY = 3rdparty
BUNDLED_DIR = 3rdparty
else
MAYBE_3RDPARTY =
BUNDLED_DIR = ..
endif

# NOTE: The stout headers were moved to the Makefile in "include"
# subdirectory to make it easy to preserve the directory structure of
# the header files during installation.
SUBDIRS = $(MAYBE_3RDPARTY) . include

PHONY_TARGETS =

include ../versions.am

BOOST = $(BUNDLED_DIR)/boost-$(BOOST_VERSION)
ELFIO = $(BUNDLED_DIR)/elfio-$(ELFIO_VERSION)
GLOG = $(BUNDLED_DIR)/glog-$(GLOG_VERSION)
GMOCK = $(GOOGLETEST)/googlemock
GOOGLETEST = $(BUNDLED_DIR)/googletest-release-$(GOOGLETEST_VERSION)
GTEST = $(GOOGLETEST)/googletest
LIBARCHIVE = $(BUNDLED_DIR)/libarchive-$(LIBARCHIVE_VERSION)
PICOJSON = $(BUNDLED_DIR)/picojson-$(PICOJSON_VERSION)
PROTOBUF = $(BUNDLED_DIR)/protobuf-$(PROTOBUF_VERSION)
RAPIDJSON = $(BUNDLED_DIR)/rapidjson-$(RAPIDJSON_VERSION)

# Dependencies for all-local.
BUNDLED_DEPS = # Initialize BUNDLED_DEPS so we can add to it.


# Ensure bundled software is build properly.
%-stamp:
	@cd $(BUNDLED_DIR) && $(MAKE) $(AM_MAKEFLAGS) $(@:$(BUNDLED_DIR)/%=%)

if WITH_BUNDLED_BOOST
BOOST_INCLUDE_FLAGS = -I$(BOOST)
BUNDLED_DEPS += $(BOOST)-stamp
endif

if WITH_BUNDLED_ELFIO
ELFIO_INCLUDE_FLAGS = -I$(ELFIO)
BUNDLED_DEPS += $(ELFIO)-stamp
endif

if WITH_BUNDLED_GLOG
GLOG_INCLUDE_FLAGS = -I$(GLOG)/src
LIB_GLOG = $(GLOG)/libglog.la
$(LIB_GLOG): $(GLOG)-build-stamp
BUNDLED_DEPS += $(GLOG)-build-stamp
else
LIB_GLOG = -lglog
endif

# TODO(kapil): Handle unbundled GMOCK.
if WITH_BUNDLED_GMOCK
GMOCK_INCLUDE_FLAGS = -I$(GMOCK)/include
GTEST_INCLUDE_FLAGS = -I$(GTEST)/include
LIB_GMOCK = $(BUNDLED_DIR)/libgmock.la
$(LIB_GMOCK): $(GMOCK)-build-stamp
BUNDLED_DEPS += $(GMOCK)-build-stamp
endif

PICOJSON_INCLUDE_FLAGS = -D__STDC_FORMAT_MACROS

if WITH_BUNDLED_LIBARCHIVE
LIBARCHIVE_INCLUDE_FLAGS = -I$(LIBARCHIVE)/libarchive
LIB_LIBARCHIVE = $(LIBARCHIVE)/.libs/libarchive.la
$(LIB_LIBARCHIVE): $(LIBARCHIVE)-build-stamp
BUNDLED_DEPS += $(LIBARCHIVE)-build-stamp
else
LIB_LIBARCHIVE = -larchive
endif

if WITH_BUNDLED_PICOJSON
PICOJSON_INCLUDE_FLAGS += -I$(PICOJSON)
BUNDLED_DEPS += $(PICOJSON)-stamp
endif

if WITH_BUNDLED_RAPIDJSON
RAPIDJSON_INCLUDE_FLAGS = -I$(RAPIDJSON)/include
BUNDLED_DEPS += $(RAPIDJSON)-stamp
endif

if WITH_BUNDLED_PROTOBUF
PROTOBUF_INCLUDE_FLAGS = -I$(PROTOBUF)/src
LIB_PROTOBUF = $(PROTOBUF)/src/libprotobuf.la
$(LIB_PROTOBUF): $(PROTOBUF)-build-stamp
BUNDLED_DEPS += $(PROTOBUF)-build-stamp
PROTOC = $(PROTOBUF)/src/protoc
else
LIB_PROTOBUF = -lprotobuf
PROTOC = @PROTOCOMPILER@
endif

PROTOCFLAGS = -I$(srcdir)/tests

# Targets for generating C++ protocol buffer code.
%.pb.cc %.pb.h: tests/%.proto $(BUNDLED_DEPS)
	$(PROTOC) $(PROTOCFLAGS) --cpp_out=$(builddir) $<

# Tests for stout.
check_PROGRAMS = stout-tests

stout_tests_SOURCES =			\
  tests/adaptor_tests.cpp		\
  tests/archiver_tests.cpp		\
  tests/base64_tests.cpp		\
  tests/bits_tests.cpp			\
  tests/boundedhashmap_tests.cpp	\
  tests/bytes_tests.cpp			\
  tests/cache_tests.cpp			\
  tests/cpp17_tests.cpp			\
  tests/duration_tests.cpp		\
  tests/dynamiclibrary_tests.cpp	\
  tests/error_tests.cpp			\
  tests/flags_tests.cpp			\
  tests/gzip_tests.cpp			\
  tests/hashmap_tests.cpp		\
  tests/hashset_tests.cpp		\
  tests/interval_tests.cpp		\
  tests/ip_tests.cpp			\
  tests/json_tests.cpp			\
  tests/jsonify_tests.cpp		\
  tests/lambda_tests.cpp		\
  tests/linkedhashmap_tests.cpp		\
  tests/mac_tests.cpp			\
  tests/main.cpp			\
  tests/multimap_tests.cpp		\
  tests/none_tests.cpp			\
  tests/numify_tests.cpp		\
  tests/option_tests.cpp		\
  tests/os_tests.cpp			\
  tests/os/copyfile_tests.cpp		\
  tests/os/env_tests.cpp		\
  tests/os/filesystem_tests.cpp		\
  tests/os/process_tests.cpp		\
  tests/os/rmdir_tests.cpp		\
  tests/os/sendfile_tests.cpp		\
  tests/os/signals_tests.cpp		\
  tests/os/socket_tests.cpp		\
  tests/os/strerror_tests.cpp		\
  tests/os/systems_tests.cpp		\
  tests/path_tests.cpp			\
  tests/protobuf_tests.cpp		\
  tests/protobuf_tests.proto		\
  tests/recordio_tests.cpp		\
  tests/result_tests.cpp		\
  tests/some_tests.cpp			\
  tests/strings_tests.cpp		\
  tests/subcommand_tests.cpp		\
  tests/svn_tests.cpp			\
  tests/try_tests.cpp			\
  tests/uri_tests.cpp			\
  tests/uuid_tests.cpp			\
  tests/variant_tests.cpp		\
  tests/version_tests.cpp

if OS_LINUX
  stout_tests_SOURCES += tests/proc_tests.cpp
endif

# The order of include flags shouldn't matter because we are allowed to disable
# some bundled dependencies in favor of system-installed ones. This would be a
# concern if some headers were to have the same paths.
stout_tests_CPPFLAGS =			\
  -I$(srcdir)/include			\
  $(BOOST_INCLUDE_FLAGS)		\
  $(ELFIO_INCLUDE_FLAGS)		\
  $(GLOG_INCLUDE_FLAGS)			\
  $(GMOCK_INCLUDE_FLAGS)		\
  $(GTEST_INCLUDE_FLAGS)		\
  $(LIBARCHIVE_INCLUDE_FLAGS)		\
  $(PICOJSON_INCLUDE_FLAGS)		\
  $(PROTOBUF_INCLUDE_FLAGS)		\
  $(RAPIDJSON_INCLUDE_FLAGS)		\
  $(AM_CPPFLAGS)

# The order of LDADD flags would matter if the same symbol is provided by
# multiple of these libraries. This is highly unlikely, especially, since we are
# allowed to disable some bundled libraries in lieu of system-installed ones.
stout_tests_LDADD =			\
  $(LIB_GMOCK)				\
  $(LIB_GLOG)				\
  $(LIB_LIBARCHIVE)			\
  $(LIB_PROTOBUF)			\
  -lsvn_subr-1				\
  -lsvn_delta-1				\
  -lapr-1

# FreeBSD makes the dynamic linking interface available in libc
if !OS_FREEBSD
  stout_tests_LDADD += -ldl
endif

BUILT_SOURCES = $(BUNDLED_DEPS)

PROTOBUF_TESTS_PROTOS =			\
  protobuf_tests.pb.cc			\
  protobuf_tests.pb.h

BUILT_SOURCES +=			\
  $(PROTOBUF_TESTS_PROTOS)

nodist_stout_tests_SOURCES =		\
  $(PROTOBUF_TESTS_PROTOS)

CLEANFILES =				\
  $(PROTOBUF_TESTS_PROTOS)

# We use a check-local target for now to avoid the parallel test
# runner that ships with newer versions of autotools.
# See the following discussion for the workaround:
# http://lists.gnu.org/archive/html/automake/2013-01/msg00051.html
check-local: tests
	$(TEST_DRIVER) ./stout-tests

tests: $(BUNDLED_DEPS) $(BUILT_SOURCES)
	$(MAKE) $(AM_MAKEFLAGS) stout-tests

PHONY_TARGETS += tests

all-local: Makefile

EXTRA_DIST = $(stout_tests_SOURCES)
if !OS_LINUX
EXTRA_DIST += tests/proc_tests.cpp
endif

# Make sure we don't recurse into 3rdparty when creating the distribution
# tarball.
# When recursing into 3rdparty, it tries to distribute the bundled dependencies
# such as boost and glog even though they have already been included as part of
# top-level 3rdparty.
DIST_SUBDIRS=$(SUBDIRS)

# Explicitly include 3rdparty directory in the distribution tarball since we
# exclude it from DIST_SUBDIRS (when building it as part of Mesos).
dist-hook:
if !STANDALONE_STOUT
	cp -r $(srcdir)/3rdparty $(distdir)/
endif

.PHONY: $(PHONY_TARGETS)
