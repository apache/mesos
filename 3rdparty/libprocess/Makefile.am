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

# Makefile for libprocess. Note that 3rdparty needs to be built
# first (see 3rdparty/Makefile.am).

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

ACLOCAL_AMFLAGS = -I m4

AUTOMAKE_OPTIONS = foreign subdir-objects
LIBPROCESS_BUILD_DIR=@abs_builddir@

if STANDALONE_LIBPROCESS
MAYBE_3RDPARTY = 3rdparty
BUNDLED_DIR = 3rdparty
else
MAYBE_3RDPARTY =
BUNDLED_DIR = ..
endif

# NOTE: The libprocess headers were moved to the Makefile in "include"
# subdirectory to make it easy to preserve the directory structure of
# the header files during installation.
SUBDIRS = $(MAYBE_3RDPARTY) . include

include ../versions.am

STOUT = ../stout

BOOST = $(BUNDLED_DIR)/boost-$(BOOST_VERSION)
CONCURRENTQUEUE = $(BUNDLED_DIR)/concurrentqueue-$(CONCURRENTQUEUE_VERSION)
ELFIO = $(BUNDLED_DIR)/elfio-$(ELFIO_VERSION)
GLOG = $(BUNDLED_DIR)/glog-$(GLOG_VERSION)
GOOGLETEST = $(BUNDLED_DIR)/googletest-release-$(GOOGLETEST_VERSION)
GMOCK = $(GOOGLETEST)/googlemock
GPERFTOOLS = $(BUNDLED_DIR)/gperftools-$(GPERFTOOLS_VERSION)
GRPC = $(BUNDLED_DIR)/grpc-$(GRPC_VERSION)
GTEST = $(GOOGLETEST)/googletest
HTTP_PARSER = $(BUNDLED_DIR)/http-parser-$(HTTP_PARSER_VERSION)
LIBEV = $(BUNDLED_DIR)/libev-$(LIBEV_VERSION)
LIBEVENT = $(BUNDLED_DIR)/libevent-$(LIBEVENT_VERSION)
PICOJSON = $(BUNDLED_DIR)/picojson-$(PICOJSON_VERSION)
PROTOBUF = $(BUNDLED_DIR)/protobuf-$(PROTOBUF_VERSION)
RAPIDJSON = $(BUNDLED_DIR)/rapidjson-$(RAPIDJSON_VERSION)

# Dependencies for all-local.
BUNDLED_DEPS = # Initialize BUNDLED_DEPENDENCIES so we can add to it.


# Ensure bundled software is build properly.
%-stamp:
	@cd $(BUNDLED_DIR) && $(MAKE) $(AM_MAKEFLAGS) $(@:$(BUNDLED_DIR)/%=%)

if WITH_BUNDLED_BOOST
BOOST_INCLUDE_FLAGS = -I$(BOOST)
BUNDLED_DEPS += $(BOOST)-stamp
endif

if WITH_BUNDLED_CONCURRENTQUEUE
CONCURRENTQUEUE_INCLUDE_FLAGS = -I$(CONCURRENTQUEUE)
BUNDLED_DEPS += $(CONCURRENTQUEUE)-stamp
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

if WITH_BUNDLED_GMOCK
GMOCK_INCLUDE_FLAGS = -I$(GMOCK)/include
GTEST_INCLUDE_FLAGS = -I$(GTEST)/include
LIB_GMOCK = $(BUNDLED_DIR)/libgmock.la
$(LIB_GMOCK): $(GMOCK)-build-stamp
BUNDLED_DEPS += $(GMOCK)-build-stamp
endif

if HAS_GPERFTOOLS
GPERFTOOLS_INCLUDE_FLAGS = -I$(GPERFTOOLS)/src
LIB_GPERFTOOLS = $(GPERFTOOLS)/libprofiler.la
$(LIB_GPERFTOOLS): $(GPERFTOOLS)-build-stamp
BUNDLED_DEPS += $(GPERFTOOLS)-build-stamp
endif

if !ENABLE_SSL
GRPC_VARIANT = _unsecure
endif

if WITH_BUNDLED_GRPC
GRPC_INCLUDE_FLAGS = -I$(GRPC)/include
LIB_GRPC = $(GRPC)/libs/opt/libgrpc++$(GRPC_VARIANT).a	\
           $(GRPC)/libs/opt/libgrpc$(GRPC_VARIANT).a	\
           $(GRPC)/libs/opt/libgpr.a
GRPC_CPP_PLUGIN = $(GRPC)/bins/opt/grpc_cpp_plugin

# NOTE: We need to specify the dependency between Protobuf and gRPC
# here, otherwise `make` might launch concurrent gRPC and Protobuf
# builds, which in turn trigger two racy Protobuf builds.
$(GRPC)-build-stamp: $(PROTOBUF)-build-stamp

$(LIB_GRPC): $(GRPC)-build-stamp
BUNDLED_DEPS += $(GRPC)-build-stamp
else
LIB_GRPC = -lgrpc++$(GRPC_VARIANT) -lgrpc$(GRPC_VARIANT) -lgpr
GRPC_CPP_PLUGIN = @GRPC_CXX_PLUGIN@
endif

if WITH_BUNDLED_HTTP_PARSER
HTTP_PARSER_INCLUDE_FLAGS = -I$(HTTP_PARSER)
LIB_HTTP_PARSER = $(BUNDLED_DIR)/libry_http_parser.la
$(LIB_HTTP_PARSER): $(HTTP_PARSER)-build-stamp
BUNDLED_DEPS += $(HTTP_PARSER)-build-stamp
else
LIB_HTTP_PARSER = -lhttp_parser
endif

if !ENABLE_LIBEVENT
if WITH_BUNDLED_LIBEV
LIB_EV_INCLUDE_FLAGS = -I$(LIBEV)
LIB_EV = $(LIBEV)/libev.la
$(LIBEV): $(LIBEV)-build-stamp
BUNDLED_DEPS += $(LIBEV)-build-stamp
else
LIB_EV = -lev
endif
else
if WITH_BUNDLED_LIBEVENT
LIB_EVENT_INCLUDE_FLAGS = -I$(LIBEVENT)/include
LIB_EVENT = $(LIBEVENT)/libevent_core.la $(LIBEVENT)/libevent_pthreads.la
if ENABLE_SSL
LIB_EVENT += $(LIBEVENT)/libevent_openssl.la
endif
$(LIBEVENT): $(LIBEVENT)-build-stamp
BUNDLED_DEPS += $(LIBEVENT)-build-stamp
else
LIB_EVENT = -levent
endif
endif

PICOJSON_INCLUDE_FLAGS = -D__STDC_FORMAT_MACROS
if WITH_BUNDLED_PICOJSON
PICOJSON_INCLUDE_FLAGS += -I$(PICOJSON)
BUNDLED_DEPS += $(PICOJSON)-stamp
endif

if WITH_BUNDLED_PROTOBUF
PROTOBUF_INCLUDE_FLAGS = -I$(PROTOBUF)/src
LIB_PROTOBUF = $(PROTOBUF)/src/libprotobuf.la
PROTOC = $(PROTOBUF)/src/protoc
$(LIB_PROTOBUF): $(PROTOBUF)-build-stamp
BUNDLED_DEPS += $(PROTOBUF)-build-stamp
else
LIB_PROTOBUF = -lprotobuf
PROTOC = @PROTOCOMPILER@
endif

if WITH_BUNDLED_RAPIDJSON
RAPIDJSON_INCLUDE_FLAGS = -I$(RAPIDJSON)/include
BUNDLED_DEPS += $(RAPIDJSON)-stamp
endif

if WITH_BUNDLED_STOUT
STOUT_INCLUDE_FLAGS = -I$(srcdir)/$(STOUT)/include
endif


# Library. It is not installable presently because most people link
# the libprocess statically into their resulting library or binary and
# don't want any parts of libprocess to get installed (which happens
# even if you attempt to do conditional installation via configure
# arguments, see configure.ac).
noinst_LTLIBRARIES = libprocess.la

libprocess_la_SOURCES =		\
  src/authenticator.cpp		\
  src/authenticator_manager.cpp	\
  src/authenticator_manager.hpp	\
  src/clock.cpp			\
  src/config.hpp		\
  src/decoder.hpp		\
  src/encoder.hpp		\
  src/event_loop.hpp		\
  src/event_queue.hpp		\
  src/firewall.cpp		\
  src/gate.hpp			\
  src/grpc.cpp			\
  src/gtest_constants.cpp	\
  src/help.cpp			\
  src/http.cpp			\
  src/http_proxy.cpp		\
  src/http_proxy.hpp		\
  src/io.cpp			\
  src/io_internal.hpp		\
  src/latch.cpp			\
  src/logging.cpp		\
  src/memory_profiler.cpp	\
  src/memory_profiler.hpp	\
  src/metrics/metrics.cpp	\
  src/mime.cpp			\
  src/mpsc_linked_queue.hpp	\
  src/pid.cpp			\
  src/posix/io.cpp		\
  src/posix/poll_socket.cpp	\
  src/posix/subprocess.cpp	\
  src/posix/subprocess.hpp	\
  src/poll_socket.hpp		\
  src/process.cpp		\
  src/process_reference.hpp	\
  src/profiler.cpp		\
  src/reap.cpp			\
  src/run_queue.hpp		\
  src/semaphore.hpp		\
  src/socket.cpp		\
  src/socket_manager.hpp	\
  src/subprocess.cpp		\
  src/time.cpp

if ENABLE_SSL
libprocess_la_SOURCES +=			\
  src/jwt.cpp					\
  src/jwt_authenticator.cpp			\
  src/openssl.cpp				\
  src/openssl.hpp				\
  src/ssl/utilities.cpp

if ENABLE_LIBEVENT
libprocess_la_SOURCES +=			\
  src/posix/libevent/libevent_ssl_socket.cpp	\
  src/posix/libevent/libevent_ssl_socket.hpp
else
libprocess_la_SOURCES +=			\
  src/ssl/openssl_socket.cpp			\
  src/ssl/openssl_socket.hpp
endif

endif


libprocess_la_CPPFLAGS =			\
  -DBUILD_DIR=\"$(LIBPROCESS_BUILD_DIR)\"	\
  -I$(srcdir)/include				\
  -I$(srcdir)/src				\
  $(BOOST_INCLUDE_FLAGS)			\
  $(CONCURRENTQUEUE_INCLUDE_FLAGS)		\
  $(ELFIO_INCLUDE_FLAGS)			\
  $(GLOG_INCLUDE_FLAGS)				\
  $(GPERFTOOLS_INCLUDE_FLAGS)			\
  $(GRPC_INCLUDE_FLAGS)				\
  $(HTTP_PARSER_INCLUDE_FLAGS)			\
  $(LIB_EV_INCLUDE_FLAGS)			\
  $(LIB_EVENT_INCLUDE_FLAGS)			\
  $(PICOJSON_INCLUDE_FLAGS)			\
  $(PROTOBUF_INCLUDE_FLAGS)			\
  $(RAPIDJSON_INCLUDE_FLAGS)			\
  $(STOUT_INCLUDE_FLAGS)			\
  $(AM_CPPFLAGS)

if ENABLE_LIBEVENT
libprocess_la_SOURCES +=			\
  src/posix/libevent/libevent.hpp		\
  src/posix/libevent/libevent.cpp		\
  src/posix/libevent/libevent_poll.cpp
else
libprocess_la_SOURCES +=			\
  src/posix/libev/libev.hpp			\
  src/posix/libev/libev.cpp			\
  src/posix/libev/libev_poll.cpp
endif

if ENABLE_STATIC_LIBPROCESS
# A static libprocess with position independent code can be used to produce a
# final shared library (e.g., libmesos.so) which includes everything necessary
# (and only install that).
libprocess_la_LDFLAGS = -static -fpic
endif

if OS_LINUX
libprocess_la_CPPFLAGS += -DLIBPROCESS_ALLOW_JEMALLOC
endif

libprocess_la_LIBADD =		\
  $(LIB_GLOG)			\
  $(LIB_GPERFTOOLS)		\
  $(LIB_HTTP_PARSER)		\
  $(LIB_EV)			\
  $(LIB_EVENT)

# Tests.
check_PROGRAMS = libprocess-tests benchmarks

libprocess_tests_SOURCES =					\
  src/tests/after_tests.cpp					\
  src/tests/collect_tests.cpp					\
  src/tests/count_down_latch_tests.cpp				\
  src/tests/decoder_tests.cpp					\
  src/tests/encoder_tests.cpp					\
  src/tests/future_tests.cpp					\
  src/tests/grpc_tests.cpp					\
  src/tests/grpc_tests.proto					\
  src/tests/http_tests.cpp					\
  src/tests/io_tests.cpp					\
  src/tests/limiter_tests.cpp					\
  src/tests/loop_tests.cpp					\
  src/tests/main.cpp						\
  src/tests/metrics_tests.cpp					\
  src/tests/mpsc_linked_queue_tests.cpp				\
  src/tests/mutex_tests.cpp					\
  src/tests/owned_tests.cpp					\
  src/tests/process_tests.cpp					\
  src/tests/profiler_tests.cpp					\
  src/tests/queue_tests.cpp					\
  src/tests/reap_tests.cpp					\
  src/tests/rwlock_tests.cpp					\
  src/tests/socket_tests.cpp					\
  src/tests/sequence_tests.cpp					\
  src/tests/shared_tests.cpp					\
  src/tests/state_machine_tests.cpp				\
  src/tests/statistics_tests.cpp				\
  src/tests/subprocess_tests.cpp				\
  src/tests/system_tests.cpp					\
  src/tests/timeseries_tests.cpp				\
  src/tests/time_tests.cpp

GRPC_TESTS_PROTOS =			\
  grpc_tests.grpc.pb.cc			\
  grpc_tests.grpc.pb.h			\
  grpc_tests.pb.cc			\
  grpc_tests.pb.h

libprocess_tests_CPPFLAGS =		\
  -I$(srcdir)/src			\
  $(GMOCK_INCLUDE_FLAGS)		\
  $(GTEST_INCLUDE_FLAGS)		\
  $(libprocess_la_CPPFLAGS)

libprocess_tests_LDADD =		\
  $(LIB_GRPC)				\
  $(LIB_GMOCK)				\
  $(LIB_PROTOBUF)			\
  libprocess.la

if ENABLE_SSL
check_PROGRAMS += ssl-client
ssl_client_SOURCES = src/tests/ssl_client.cpp
ssl_client_CPPFLAGS = $(libprocess_tests_CPPFLAGS)
ssl_client_LDADD = $(libprocess_tests_LDADD)

libprocess_tests_SOURCES +=		\
  src/tests/jwt_tests.cpp		\
  src/tests/ssl_tests.cpp
endif

benchmarks_SOURCES =			\
  src/tests/benchmarks.cpp		\
  src/tests/benchmarks.proto

BENCHMARKS_PROTOS =			\
  benchmarks.pb.cc			\
  benchmarks.pb.h

benchmarks_CPPFLAGS =			\
  -I$(srcdir)/src			\
  $(GMOCK_INCLUDE_FLAGS)		\
  $(GTEST_INCLUDE_FLAGS)		\
  $(libprocess_la_CPPFLAGS)

benchmarks_LDADD =			\
  $(LIB_GMOCK)				\
  $(LIB_PROTOBUF)			\
  libprocess.la

# Used for testing remote links.
check_PROGRAMS += test-linkee
test_linkee_SOURCES = src/tests/test_linkee.cpp
test_linkee_CPPFLAGS = $(libprocess_tests_CPPFLAGS)
test_linkee_LDADD = $(libprocess_tests_LDADD)

# Compile the example to make sure it remains valid.
check_PROGRAMS += example
example_SOURCES = examples/example.cpp
example_CPPFLAGS = $(libprocess_tests_CPPFLAGS)
example_LDADD = $(libprocess_tests_LDADD)

if !ENABLE_STATIC_LIBPROCESS
# If libprocess is not static, we need to directly link against libglog, etc.
# When libprocess is configured by Mesos, it is forced to generate a static
# libprocess.a library. The static libprocess.a encapsulates libglog, etc.
# However, when building a standalone libprocess, we need to explicitly link
# these tests against libglog, etc., because libprocess.so won't contain these
# libraries.
benchmarks_LDADD +=			\
  $(LIB_GLOG)				\
  $(LIB_HTTP_PARSER)			\
  $(LIB_EV)				\
  $(LIB_EVENT)

libprocess_tests_LDADD +=		\
  $(LIB_GLOG)				\
  $(LIB_HTTP_PARSER)			\
  $(LIB_EV)				\
  $(LIB_EVENT)
endif

BUILT_SOURCES =				\
  $(BUNDLED_DEPS)			\
  $(BENCHMARKS_PROTOS)			\
  $(GRPC_TESTS_PROTOS)

nodist_benchmarks_SOURCES =		\
  $(BENCHMARKS_PROTOS)

nodist_libprocess_tests_SOURCES =	\
  $(GRPC_TESTS_PROTOS)

CLEANFILES =				\
  $(BENCHMARKS_PROTOS)			\
  $(GRPC_TESTS_PROTOS)


# Targets for generating C++ protocol buffer and gRPC code for tests.
%.pb.cc %.pb.h: src/tests/%.proto $(BUNDLED_DEPS)
	$(PROTOC) -I$(<D) --cpp_out=. $<

%.grpc.pb.cc %.grpc.pb.h: src/tests/%.proto $(BUNDLED_DEPS)
	$(PROTOC) -I$(<D) --grpc_out=. $<			\
	          --plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN)


# We use a check-local target for now to avoid the parallel test
# runner that ships with newer versions of autotools.
# See the following discussion for the workaround:
# http://lists.gnu.org/archive/html/automake/2013-01/msg00051.html
check-local: tests
	$(TEST_DRIVER) ./libprocess-tests

tests: all $(BUNDLED_DEPS) $(BENCHMARKS_PROTOS) $(GRPC_TESTS_PROTOS)
	$(MAKE) $(AM_MAKEFLAGS) $(check_PROGRAMS)

# TODO(benh): Fix shared builds (tests need libglog, libev, etc).


# Make sure we don't recurse into 3rdparty when creating the distribution
# tarball.
# When recursing into 3rdparty, it tries to distribute the bundled dependencies
# such as boost and glog even though they have already been included as part of
# top-level 3rdparty.
DIST_SUBDIRS=$(SUBDIRS)

# Explicitly include 3rdparty directory in the distribution tarball since we
# exclude it from DIST_SUBDIRS (when building it as part of Mesos).
dist-hook:
if !STANDALONE_LIBPROCESS
	cp -r $(srcdir)/3rdparty $(distdir)/
endif

# Using LT_OUTPUT in configure.ac creates config.lt that doesn't get
# cleaned up by distclean-libtool. See this bug patch (which doesn't
# appear to be in all versions of libtool.m4):
# http://lists.gnu.org/archive/html/automake-commit/2008-11/msg00015.html.
distclean-local:
	-rm -f config.lt
