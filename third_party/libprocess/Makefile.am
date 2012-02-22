# Makefile for libprocess. Note that third_party needs to be built
# first (see third_party/Makefile.am).

ACLOCAL_AMFLAGS = -I m4

AUTOMAKE_OPTIONS = foreign

SUBDIRS = third_party .

include third_party/versions.am

BOOST = third_party/boost-$(BOOST_VERSION)
GLOG = third_party/glog-$(GLOG_VERSION)
GMOCK = third_party/gmock-$(GMOCK_VERSION)
GTEST = $(GMOCK)/gtest
RY_HTTP_PARSER = third_party/ry-http-parser-$(RY_HTTP_PARSER_VERSION)
LIBEV = third_party/libev-$(LIBEV_VERSION)


# Library.
lib_LTLIBRARIES = libprocess.la
libprocess_la_SOURCES = src/process.cpp src/pid.cpp src/latch.cpp	\
	src/tokenize.cpp src/config.hpp src/decoder.hpp			\
	src/encoder.hpp src/foreach.hpp src/gate.hpp			\
	src/synchronized.hpp src/thread.hpp src/tokenize.hpp
libprocess_la_CPPFLAGS = -I$(srcdir)/include -I$(BOOST) -I$(GLOG)/src	\
	-I$(RY_HTTP_PARSER) -I$(LIBEV) $(AM_CPPFLAGS)
libprocess_la_LIBADD = $(GLOG)/.libs/libglog.la				\
	third_party/libry_http_parser.la $(LIBEV)/.libs/libev.la


# Headers.
pkginclude_HEADERS = include/process/clock.hpp				\
	include/process/collect.hpp include/process/defer.hpp		\
	include/process/deferred.hpp include/process/dispatch.hpp	\
	include/process/event.hpp include/process/executor.hpp		\
	include/process/filter.hpp include/process/future.hpp		\
	include/process/gc.hpp include/process/http.hpp			\
	include/process/latch.hpp include/process/message.hpp		\
	include/process/option.hpp include/process/pid.hpp		\
	include/process/preprocessor.hpp include/process/process.hpp	\
	include/process/protobuf.hpp include/process/run.hpp		\
	include/process/socket.hpp include/process/timeout.hpp		\
	include/process/timer.hpp


# Tests.
check_PROGRAMS = tests

tests_SOURCES = src/tests.cpp
tests_CPPFLAGS = -I$(GTEST)/include -I$(GMOCK)/include $(libprocess_la_CPPFLAGS)
tests_LDADD = third_party/libgmock.la libprocess.la
