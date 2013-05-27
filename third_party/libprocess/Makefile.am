# Makefile for libprocess. Note that third_party needs to be built
# first (see third_party/Makefile.am).

ACLOCAL_AMFLAGS = -I m4

AUTOMAKE_OPTIONS = foreign

SUBDIRS = third_party .

include third_party/versions.am

STOUT = third_party/stout
BOOST = third_party/boost-$(BOOST_VERSION)
GLOG = third_party/glog-$(GLOG_VERSION)
GMOCK = third_party/gmock-$(GMOCK_VERSION)
GPERFTOOLS = third_party/gperftools-$(GPERFTOOLS_VERSION)
GTEST = $(GMOCK)/gtest
RY_HTTP_PARSER = third_party/ry-http-parser-$(RY_HTTP_PARSER_VERSION)
LIBEV = third_party/libev-$(LIBEV_VERSION)


# Library. It is not installable presently because most people link
# the libprocess statically into their resulting library or binary and
# don't want any parts of libprocess to get installed (which happens
# even if you attempt to do conditional installation via configure
# arguments, see configure.ac).
noinst_LTLIBRARIES = libprocess.la

libprocess_la_SOURCES =		\
  src/config.hpp		\
  src/decoder.hpp		\
  src/encoder.hpp		\
  src/gate.hpp			\
  src/latch.cpp			\
  src/pid.cpp			\
  src/process.cpp		\
  src/statistics.cpp		\
  src/synchronized.hpp

libprocess_la_CPPFLAGS =		\
  -I$(srcdir)/include			\
  -I$(srcdir)/$(STOUT)/include		\
  -I$(BOOST)				\
  -I$(GLOG)/src				\
  -I$(LIBEV)				\
  -I$(RY_HTTP_PARSER)			\
  $(AM_CPPFLAGS)

libprocess_la_LIBADD =			\
  $(GLOG)/libglog.la			\
  third_party/libry_http_parser.la	\
  $(LIBEV)/libev.la

if HAS_GPERFTOOLS
libprocess_la_CPPFLAGS += -I$(GPERFTOOLS)/src
libprocess_la_LIBADD += $(GPERFTOOLS)/libprofiler.la
endif

# Headers.
libprocess_la_SOURCES +=					\
  $(top_srcdir)/include/process/async.hpp			\
  $(top_srcdir)/include/process/clock.hpp			\
  $(top_srcdir)/include/process/collect.hpp			\
  $(top_srcdir)/include/process/defer.hpp			\
  $(top_srcdir)/include/process/deferred.hpp			\
  $(top_srcdir)/include/process/delay.hpp			\
  $(top_srcdir)/include/process/dispatch.hpp			\
  $(top_srcdir)/include/process/event.hpp			\
  $(top_srcdir)/include/process/executor.hpp			\
  $(top_srcdir)/include/process/filter.hpp			\
  $(top_srcdir)/include/process/future.hpp			\
  $(top_srcdir)/include/process/gc.hpp				\
  $(top_srcdir)/include/process/gmock.hpp			\
  $(top_srcdir)/include/process/gtest.hpp			\
  $(top_srcdir)/include/process/http.hpp			\
  $(top_srcdir)/include/process/id.hpp				\
  $(top_srcdir)/include/process/io.hpp				\
  $(top_srcdir)/include/process/latch.hpp			\
  $(top_srcdir)/include/process/logging.hpp			\
  $(top_srcdir)/include/process/message.hpp			\
  $(top_srcdir)/include/process/mime.hpp			\
  $(top_srcdir)/include/process/once.hpp			\
  $(top_srcdir)/include/process/pid.hpp				\
  $(top_srcdir)/include/process/process.hpp			\
  $(top_srcdir)/include/process/profiler.hpp			\
  $(top_srcdir)/include/process/protobuf.hpp			\
  $(top_srcdir)/include/process/run.hpp				\
  $(top_srcdir)/include/process/socket.hpp			\
  $(top_srcdir)/include/process/statistics.hpp			\
  $(top_srcdir)/include/process/thread.hpp			\
  $(top_srcdir)/include/process/time.hpp			\
  $(top_srcdir)/include/process/timeout.hpp			\
  $(top_srcdir)/include/process/timer.hpp

# Tests.
check_PROGRAMS = tests

tests_SOURCES =							\
  src/tests/decoder_tests.cpp					\
  src/tests/encoder_tests.cpp					\
  src/tests/http_tests.cpp					\
  src/tests/io_tests.cpp					\
  src/tests/main.cpp						\
  src/tests/process_tests.cpp					\
  src/tests/statistics_tests.cpp				\
  src/tests/time_tests.cpp

tests_CPPFLAGS =			\
  -I$(top_srcdir)/src			\
  -I$(GTEST)/include			\
  -I$(GMOCK)/include			\
  $(libprocess_la_CPPFLAGS)

tests_LDADD = third_party/libgmock.la libprocess.la

TESTS = tests

# TODO(benh): Fix shared builds (tests need libglog, libev, etc).

# Using LT_OUTPUT in configure.ac creates config.lt that doesn't get
# cleaned up by distclean-libtool. See this bug patch (which doesn't
# appear to be in all versions of libtool.m4):
# http://lists.gnu.org/archive/html/automake-commit/2008-11/msg00015.html.
distclean-local:
	-rm -f config.lt
