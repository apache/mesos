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

include ../../versions.am

STOUT = stout
BOOST = ../../boost-$(BOOST_VERSION)
GLOG = ../../glog-$(GLOG_VERSION)
GMOCK = ../../gmock-$(GMOCK_VERSION)
GTEST = $(GMOCK)/gtest
PROTOBUF = ../../protobuf-$(PROTOBUF_VERSION)
PICOJSON = ../../picojson-$(PICOJSON_VERSION)

ALL_LOCAL = # Initialize ALL_LOCAL so we can add to it.

# We use "-isystem" instead of "-I" to add Boost to the include search
# path. This disables compiler warnings inside Boost headers since we
# can't easily fix them. See MESOS-3799.
if WITH_BUNDLED_BOOST
BOOST_INCLUDE_FLAGS = -isystem $(BOOST)
endif

if WITH_BUNDLED_GLOG
GLOG_INCLUDE_FLAGS = -I$(GLOG)/src
LIB_GLOG = $(GLOG)/libglog.la
else
LIB_GLOG = -lglog
endif

# TODO(kapil): Handle unbundled GMOCK.
if WITH_BUNDLED_GMOCK
GMOCK_INCLUDE_FLAGS = -I$(GMOCK)/include
GTEST_INCLUDE_FLAGS = -I$(GTEST)/include
LIB_GMOCK = ../../libgmock.la
endif

# Enable the INT64 support for PicoJSON.
PICOJSON_INCLUDE_FLAGS =	\
  -DPICOJSON_USE_INT64		\
  -D__STDC_FORMAT_MACROS

if WITH_BUNDLED_PICOJSON
PICOJSON_INCLUDE_FLAGS += -I$(PICOJSON)
endif

if WITH_BUNDLED_PROTOBUF
PROTOBUF_INCLUDE_FLAGS = -I$(PROTOBUF)/src
LIB_PROTOBUF = $(PROTOBUF)/src/libprotobuf.la
else
LIB_PROTOBUF = -lprotobuf
endif

if WITH_BUNDLED_STOUT

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
  $(STOUT)/tests/ip_tests.cpp			\
  $(STOUT)/tests/json_tests.cpp			\
  $(STOUT)/tests/jsonify_tests.cpp		\
  $(STOUT)/tests/linkedhashmap_tests.cpp	\
  $(STOUT)/tests/mac_tests.cpp			\
  $(STOUT)/tests/main.cpp			\
  $(STOUT)/tests/multimap_tests.cpp		\
  $(STOUT)/tests/none_tests.cpp			\
  $(STOUT)/tests/numify_tests.cpp		\
  $(STOUT)/tests/option_tests.cpp		\
  $(STOUT)/tests/os_tests.cpp			\
  $(STOUT)/tests/os/env_tests.cpp		\
  $(STOUT)/tests/os/filesystem_tests.cpp	\
  $(STOUT)/tests/os/process_tests.cpp		\
  $(STOUT)/tests/os/rmdir_tests.cpp		\
  $(STOUT)/tests/os/sendfile_tests.cpp		\
  $(STOUT)/tests/os/signals_tests.cpp		\
  $(STOUT)/tests/os/socket_tests.cpp		\
  $(STOUT)/tests/os/strerror_tests.cpp		\
  $(STOUT)/tests/os/systems_tests.cpp		\
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

# The order of include flags shouldn't matter because we are allowed to disable
# some bundled dependencies in favor of system-installed ones. This would be a
# concern if some headers were to have the same paths.
stout_tests_CPPFLAGS =				\
  -I$(srcdir)/$(STOUT)/include			\
  $(BOOST_INCLUDE_FLAGS)			\
  $(GLOG_INCLUDE_FLAGS)				\
  $(GMOCK_INCLUDE_FLAGS)			\
  $(GTEST_INCLUDE_FLAGS)			\
  $(PICOJSON_INCLUDE_FLAGS)			\
  $(PROTOBUF_INCLUDE_FLAGS)			\
  $(AM_CPPFLAGS)

# The order of LDADD flags would matter if the same symbol is provided by
# multiple of these libraries. This is highly unlikely, especially, since we are
# allowed to disable some bundled libraries in lieu of system-installed ones.
stout_tests_LDADD =			\
  $(LIB_GMOCK)				\
  $(LIB_GLOG)				\
  $(LIB_PROTOBUF)			\
  -lsvn_subr-1				\
  -lsvn_delta-1				\
  -lapr-1

# FreeBSD makes the dynamic linking interface available in libc
if !OS_FREEBSD
  stout_tests_LDADD += -ldl
endif

endif # if WITH_BUNDLED_STOUT

# Both libprocess-tests and benchmarks have a dependency on libgmock.la.
$(LIB_GMOCK):
	@cd ../.. && $(MAKE) $(AM_MAKEFLAGS) libgmock.la

# We use a check-local target for now to avoid the parallel test
# runner that ships with newer versions of autotools.
# See the following discussion for the workaround:
# http://lists.gnu.org/archive/html/automake/2013-01/msg00051.html
check-local: tests
if WITH_BUNDLED_STOUT
	./stout-tests
endif

tests: all
if WITH_BUNDLED_STOUT
	$(MAKE) $(AM_MAKEFLAGS) stout-tests
endif

if WITH_BUNDLED_STOUT
# Dependencies for all-local.
ALL_LOCAL += $(STOUT)/Makefile
endif

all-local: $(ALL_LOCAL)
