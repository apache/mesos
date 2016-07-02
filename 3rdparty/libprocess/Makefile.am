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

ACLOCAL_AMFLAGS = -I m4

AUTOMAKE_OPTIONS = foreign
LIBPROCESS_BUILD_DIR=@abs_builddir@

# NOTE: The libprocess headers were moved to the Makefile in "include"
# subdirectory to make it easy to preserve the directory structure of
# the header files during installation.
SUBDIRS = . include

include ../versions.am

STOUT = ../stout
BOOST = ../boost-$(BOOST_VERSION)
ELFIO = ../elfio-$(ELFIO_VERSION)
GLOG = ../glog-$(GLOG_VERSION)
GMOCK = ../gmock-$(GMOCK_VERSION)
GPERFTOOLS = ../gperftools-$(GPERFTOOLS_VERSION)
GTEST = $(GMOCK)/gtest
LIBEV = ../libev-$(LIBEV_VERSION)
PICOJSON = ../picojson-$(PICOJSON_VERSION)


if WITH_BUNDLED_BOOST
BOOST_INCLUDE_FLAGS = -isystem $(BOOST)
endif

if WITH_BUNDLED_ELFIO
ELFIO_INCLUDE_FLAGS = -I$(ELFIO)
endif

if WITH_BUNDLED_GLOG
GLOG_INCLUDE_FLAGS = -I$(GLOG)/src
LIB_GLOG = $(GLOG)/libglog.la
else
LIB_GLOG = -lglog
endif

if WITH_BUNDLED_GMOCK
GMOCK_INCLUDE_FLAGS = -I$(GMOCK)/include
GTEST_INCLUDE_FLAGS = -I$(GTEST)/include
LIB_GMOCK = ../libgmock.la
endif

if HAS_GPERFTOOLS
GPERFTOOLS_INCLUDE_FLAGS = -I$(GPERFTOOLS)/src
LIB_GPERFTOOLS = $(GPERFTOOLS)/libprofiler.la
endif

if WITH_BUNDLED_HTTP_PARSER
HTTP_PARSER = ../http-parser-$(HTTP_PARSER_VERSION)
HTTP_PARSER_INCLUDE_FLAGS = -I$(HTTP_PARSER)
LIB_HTTP_PARSER = ../libry_http_parser.la
else
LIB_HTTP_PARSER = -lhttp_parser
endif

if !ENABLE_LIBEVENT
if WITH_BUNDLED_LIBEV
LIB_EV_INCLUDE_FLAGS = -I$(LIBEV)
LIB_EV = $(LIBEV)/libev.la
else
LIB_EV = -lev
endif
endif

PICOJSON_INCLUDE_FLAGS =	\
  -DPICOJSON_USE_INT64		\
  -D__STDC_FORMAT_MACROS
if WITH_BUNDLED_PICOJSON
PICOJSON_INCLUDE_FLAGS += -I$(PICOJSON)
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
  src/authenticator_manager.hpp	\
  src/authenticator_manager.cpp	\
  src/authenticator.cpp		\
  src/clock.cpp			\
  src/config.hpp		\
  src/decoder.hpp		\
  src/encoder.hpp		\
  src/event_loop.hpp		\
  src/firewall.cpp		\
  src/gate.hpp			\
  src/help.cpp			\
  src/http.cpp			\
  src/io.cpp			\
  src/latch.cpp			\
  src/logging.cpp		\
  src/metrics/metrics.cpp	\
  src/pid.cpp			\
  src/poll_socket.cpp		\
  src/poll_socket.hpp		\
  src/profiler.cpp		\
  src/process.cpp		\
  src/process_reference.hpp	\
  src/reap.cpp			\
  src/socket.cpp		\
  src/subprocess.cpp		\
  src/subprocess_posix.cpp	\
  src/time.cpp			\
  src/timeseries.cpp

if ENABLE_SSL
libprocess_la_SOURCES +=	\
    src/libevent_ssl_socket.cpp	\
    src/libevent_ssl_socket.hpp	\
    src/openssl.cpp		\
    src/openssl.hpp		\
    src/ssl/utilities.cpp
endif


# We use "-isystem" instead of "-I" to add Boost to the include search
# path. This disables compiler warnings inside Boost headers since we
# can't easily fix them. See MESOS-3799.
libprocess_la_CPPFLAGS =			\
  -DBUILD_DIR=\"$(LIBPROCESS_BUILD_DIR)\"	\
  -I$(srcdir)/include				\
  $(BOOST_INCLUDE_FLAGS)			\
  $(ELFIO_INCLUDE_FLAGS)			\
  $(GLOG_INCLUDE_FLAGS)				\
  $(GPERFTOOLS_INCLUDE_FLAGS)			\
  $(HTTP_PARSER_INCLUDE_FLAGS)			\
  $(LIB_EV_INCLUDE_FLAGS)			\
  $(PICOJSON_INCLUDE_FLAGS)			\
  $(STOUT_INCLUDE_FLAGS)			\
  $(AM_CPPFLAGS)

if ENABLE_LIBEVENT
libprocess_la_SOURCES +=	\
    src/libevent.hpp		\
    src/libevent.cpp		\
    src/libevent_poll.cpp
else
  libprocess_la_SOURCES +=	\
    src/libev.hpp		\
    src/libev.cpp		\
    src/libev_poll.cpp
endif

if ENABLE_STATIC_LIBPROCESS
# A static libprocess with position independent code can be used to produce a
# final shared library (e.g., libmesos.so) which includes everything necessary
# (and only install that).
libprocess_la_LDFLAGS = -static -fpic
endif

libprocess_la_LIBADD =		\
  $(LIB_GLOG)			\
  $(LIB_GPERFTOOLS)		\
  $(LIB_HTTP_PARSER)		\
  $(LIB_EV)

# Tests.
check_PROGRAMS = libprocess-tests benchmarks

libprocess_tests_SOURCES =					\
  src/tests/collect_tests.cpp					\
  src/tests/decoder_tests.cpp					\
  src/tests/encoder_tests.cpp					\
  src/tests/future_tests.cpp					\
  src/tests/http_tests.cpp					\
  src/tests/io_tests.cpp					\
  src/tests/limiter_tests.cpp					\
  src/tests/main.cpp						\
  src/tests/metrics_tests.cpp					\
  src/tests/mutex_tests.cpp					\
  src/tests/owned_tests.cpp					\
  src/tests/process_tests.cpp					\
  src/tests/profiler_tests.cpp					\
  src/tests/queue_tests.cpp					\
  src/tests/reap_tests.cpp					\
  src/tests/sequence_tests.cpp					\
  src/tests/shared_tests.cpp					\
  src/tests/statistics_tests.cpp				\
  src/tests/subprocess_tests.cpp				\
  src/tests/system_tests.cpp					\
  src/tests/timeseries_tests.cpp				\
  src/tests/time_tests.cpp

libprocess_tests_CPPFLAGS =		\
  -I$(srcdir)/src			\
  $(GMOCK_INCLUDE_FLAGS)		\
  $(GTEST_INCLUDE_FLAGS)		\
  $(libprocess_la_CPPFLAGS)

libprocess_tests_LDADD =		\
  $(LIB_GMOCK)				\
  libprocess.la

if ENABLE_SSL
check_PROGRAMS += ssl-client
ssl_client_SOURCES = src/tests/ssl_client.cpp
ssl_client_CPPFLAGS = $(libprocess_tests_CPPFLAGS)
ssl_client_LDADD = $(libprocess_tests_LDADD)

libprocess_tests_SOURCES +=		\
  src/tests/ssl_tests.cpp
endif

benchmarks_SOURCES =			\
  src/tests/benchmarks.cpp

benchmarks_CPPFLAGS =			\
  -I$(srcdir)/src			\
  $(GMOCK_INCLUDE_FLAGS)		\
  $(GTEST_INCLUDE_FLAGS)		\
  $(libprocess_la_CPPFLAGS)

benchmarks_LDADD =			\
  $(LIB_GMOCK)				\
  libprocess.la

# Used for testing remote links.
check_PROGRAMS += test-linkee
test_linkee_SOURCES = src/tests/test_linkee.cpp
test_linkee_CPPFLAGS = $(libprocess_tests_CPPFLAGS)
test_linkee_LDADD = $(libprocess_tests_LDADD)

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
  $(LIB_EV)

libprocess_tests_LDADD +=		\
  $(LIB_GLOG)				\
  $(LIB_HTTP_PARSER)			\
  $(LIB_EV)
endif

# We use a check-local target for now to avoid the parallel test
# runner that ships with newer versions of autotools.
# See the following discussion for the workaround:
# http://lists.gnu.org/archive/html/automake/2013-01/msg00051.html
check-local: tests
	./libprocess-tests

# Both libprocess-tests and benchmarks have a dependency on libgmock.la.
$(LIB_GMOCK):
	@cd .. && $(MAKE) $(AM_MAKEFLAGS) libgmock.la

tests: all libprocess-tests benchmarks

# TODO(benh): Fix shared builds (tests need libglog, libev, etc).

# Using LT_OUTPUT in configure.ac creates config.lt that doesn't get
# cleaned up by distclean-libtool. See this bug patch (which doesn't
# appear to be in all versions of libtool.m4):
# http://lists.gnu.org/archive/html/automake-commit/2008-11/msg00015.html.
distclean-local:
	-rm -f config.lt
