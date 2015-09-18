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
LIBPROCESS_BUILD_DIR=@abs_top_builddir@

# NOTE: The libprocess headers were moved to the Makefile in "include"
# subdirectory to make it easy to preserve the directory structure of
# the header files during installation.
SUBDIRS = 3rdparty . include

include 3rdparty/versions.am

STOUT = 3rdparty/stout
BOOST = 3rdparty/boost-$(BOOST_VERSION)
GLOG = 3rdparty/glog-$(GLOG_VERSION)
GMOCK = 3rdparty/gmock-$(GMOCK_VERSION)
GPERFTOOLS = 3rdparty/gperftools-$(GPERFTOOLS_VERSION)
GTEST = $(GMOCK)/gtest
LIBEV = 3rdparty/libev-$(LIBEV_VERSION)
PICOJSON = 3rdparty/picojson-$(PICOJSON_VERSION)


# Library. It is not installable presently because most people link
# the libprocess statically into their resulting library or binary and
# don't want any parts of libprocess to get installed (which happens
# even if you attempt to do conditional installation via configure
# arguments, see configure.ac).
noinst_LTLIBRARIES = libprocess.la

libprocess_la_SOURCES =		\
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
  src/time.cpp			\
  src/timeseries.cpp

if ENABLE_LIBEVENT
else
if WITH_BUNDLED_LIBEV
  EVENT_LIB = $(LIBEV)/libev.la
else
  EVENT_LIB = -lev
endif
endif

if ENABLE_SSL
libprocess_la_SOURCES +=	\
    src/libevent_ssl_socket.cpp	\
    src/libevent_ssl_socket.hpp	\
    src/openssl.cpp		\
    src/openssl.hpp		\
    src/ssl/utilities.cpp
endif

libprocess_la_CPPFLAGS =			\
  -DBUILD_DIR=\"$(LIBPROCESS_BUILD_DIR)\"	\
  -I$(srcdir)/include				\
  -I$(srcdir)/$(STOUT)/include			\
  -I$(BOOST)					\
  -I$(LIBEV)					\
  -I$(PICOJSON)					\
  -DPICOJSON_USE_INT64				\
  -D__STDC_FORMAT_MACROS			\
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

if WITH_BUNDLED_GLOG
  libprocess_la_CPPFLAGS += -I$(GLOG)/src
  LIBGLOG = $(GLOG)/libglog.la
else
  LIBGLOG = -lglog
endif

if WITH_BUNDLED_HTTP_PARSER
  RY_HTTP_PARSER = 3rdparty/ry-http-parser-$(RY_HTTP_PARSER_VERSION)
  libprocess_la_CPPFLAGS += -I$(RY_HTTP_PARSER)

  HTTP_PARSER_LIB = 3rdparty/libry_http_parser.la
else
  HTTP_PARSER_LIB = -lhttp_parser
endif

libprocess_la_LIBADD =			\
  $(LIBGLOG)				\
  $(HTTP_PARSER_LIB)			\
  $(EVENT_LIB)

if HAS_GPERFTOOLS
libprocess_la_CPPFLAGS += -I$(GPERFTOOLS)/src
libprocess_la_LIBADD += $(GPERFTOOLS)/libprofiler.la
endif

# Tests.
check_PROGRAMS = tests benchmarks

tests_SOURCES =							\
  src/tests/collect_tests.cpp					\
  src/tests/decoder_tests.cpp					\
  src/tests/encoder_tests.cpp					\
  src/tests/future_tests.cpp					\
  src/tests/http_tests.cpp					\
  src/tests/io_tests.cpp					\
  src/tests/limiter_tests.cpp					\
  src/tests/main.cpp						\
  src/tests/mutex_tests.cpp					\
  src/tests/metrics_tests.cpp					\
  src/tests/owned_tests.cpp					\
  src/tests/process_tests.cpp					\
  src/tests/queue_tests.cpp					\
  src/tests/reap_tests.cpp					\
  src/tests/sequence_tests.cpp					\
  src/tests/shared_tests.cpp					\
  src/tests/statistics_tests.cpp				\
  src/tests/subprocess_tests.cpp				\
  src/tests/system_tests.cpp					\
  src/tests/timeseries_tests.cpp				\
  src/tests/time_tests.cpp

tests_CPPFLAGS =			\
  -I$(top_srcdir)/src			\
  -I$(GTEST)/include			\
  -I$(GMOCK)/include			\
  $(libprocess_la_CPPFLAGS)

tests_LDADD =				\
  3rdparty/libgmock.la			\
  libprocess.la				\
  $(LIBGLOG)				\
  $(HTTP_PARSER_LIB)			\
  $(EVENT_LIB)

if ENABLE_SSL
check_PROGRAMS += ssl-client
ssl_client_SOURCES = src/tests/ssl_client.cpp
ssl_client_CPPFLAGS = $(tests_CPPFLAGS)
ssl_client_LDADD = $(tests_LDADD)

tests_SOURCES +=						\
  src/tests/ssl_tests.cpp
endif

benchmarks_SOURCES =			\
  src/tests/benchmarks.cpp

benchmarks_CPPFLAGS =			\
  -I$(top_srcdir)/src			\
  -I$(GTEST)/include			\
  -I$(GMOCK)/include			\
  $(libprocess_la_CPPFLAGS)

benchmarks_LDADD =			\
  3rdparty/libgmock.la			\
  libprocess.la				\
  $(LIBGLOG)				\
  $(HTTP_PARSER_LIB)			\
  $(EVENT_LIB)

# We use a check-local target for now to avoid the parallel test
# runner that ships with newer versions of autotools.
# See the following discussion for the workaround:
# http://lists.gnu.org/archive/html/automake/2013-01/msg00051.html
check-local: tests benchmarks
	./tests

# TODO(benh): Fix shared builds (tests need libglog, libev, etc).

# Using LT_OUTPUT in configure.ac creates config.lt that doesn't get
# cleaned up by distclean-libtool. See this bug patch (which doesn't
# appear to be in all versions of libtool.m4):
# http://lists.gnu.org/archive/html/automake-commit/2008-11/msg00015.html.
distclean-local:
	-rm -f config.lt
