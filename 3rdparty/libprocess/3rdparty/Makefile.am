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
RY_HTTP_PARSER = ry-http-parser-$(RY_HTTP_PARSER_VERSION)
LIBEV = libev-$(LIBEV_VERSION)
PROTOBUF = protobuf-$(PROTOBUF_VERSION)


EXTRA_DIST =			\
  $(BOOST).tar.gz		\
  $(GLOG).tar.gz		\
  $(GMOCK).tar.gz		\
  $(GPERFTOOLS).tar.gz		\
  $(LIBEV).tar.gz		\
  $(LIBEV).patch		\
  $(PROTOBUF).tar.gz		\
  $(RY_HTTP_PARSER).tar.gz

CLEAN_EXTRACTED =	\
  $(BOOST)		\
  $(GLOG)		\
  $(GMOCK)		\
  $(GPERFTOOLS)		\
  $(LIBEV)		\
  $(PROTOBUF)		\
  $(RY_HTTP_PARSER)


# This is where the magic happens: we use stamp files as dependencies
# which cause the packages to get extracted as necessary. We also
# apply any patches as appropriate.
%-stamp: %.tar.gz
	gzip -d -c $^ | tar xf -
	test ! -e $(srcdir)/$*.patch || patch -d $* -p1 <$(srcdir)/$*.patch
	touch $@


# Convenience library for Ryan Dahl's HTTP parser.
noinst_LTLIBRARIES = libry_http_parser.la
nodist_libry_http_parser_la_SOURCES = $(RY_HTTP_PARSER)/http_parser.c
libry_http_parser_la_CPPFLAGS = -I$(RY_HTTP_PARSER)

# We list the sources in BUILT_SOURCES to make sure that the package
# gets unarchived first.
BUILT_SOURCES += $(nodist_libry_http_parser_la_SOURCES)


# Convenience library for gmock/gtest.
check_LTLIBRARIES = libgmock.la
nodist_libgmock_la_SOURCES =		\
  $(GTEST)/src/gtest-all.cc		\
  $(GMOCK)/src/gmock-all.cc
libgmock_la_CPPFLAGS =			\
  -I$(GTEST)/include -I$(GTEST)		\
  -I$(GMOCK)/include -I$(GMOCK)

# We list the sources in BUILT_SOURCES to make sure that the package
# gets unarchived first.
BUILT_SOURCES += $(nodist_libgmock_la_SOURCES)

$(GMOCK)/src/gmock-all.cc: $(GMOCK)-stamp
$(GTEST)/src/gtest-all.cc: $(GMOCK)-stamp


$(BOOST)/boost: $(BOOST)-stamp

$(GLOG)/libglog.la: $(GLOG)-stamp
	cd $(GLOG) && ./configure $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)

if HAS_GPERFTOOLS
$(GPERFTOOLS)/libprofiler.la: $(GPERFTOOLS)-build-stamp

$(GPERFTOOLS)-build-stamp: $(GPERFTOOLS)-stamp
	cd $(GPERFTOOLS) && ./configure  $(CONFIGURE_ARGS) && \
	  $(MAKE) $(AM_MAKEFLAGS)
	touch $@
endif

$(LIBEV)/libev.la: $(LIBEV)-stamp
	cd $(LIBEV) && ./configure $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)

$(PROTOBUF)/src/protoc $(PROTOBUF)/src/libprotobuf.la: $(PROTOBUF)-build-stamp

$(PROTOBUF)-build-stamp: $(PROTOBUF)-stamp
	cd $(PROTOBUF) && ./configure $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)
	touch $@

$(RY_HTTP_PARSER)/http_parser.c: $(RY_HTTP_PARSER)-stamp


# Tests for stout.
check_PROGRAMS = stout-tests

stout_tests_SOURCES =				\
  $(STOUT)/tests/bytes_tests.cpp		\
  $(STOUT)/tests/duration_tests.cpp		\
  $(STOUT)/tests/error_tests.cpp		\
  $(STOUT)/tests/flags_tests.cpp		\
  $(STOUT)/tests/gzip_tests.cpp			\
  $(STOUT)/tests/hashset_tests.cpp		\
  $(STOUT)/tests/json_tests.cpp			\
  $(STOUT)/tests/main.cpp			\
  $(STOUT)/tests/multimap_tests.cpp		\
  $(STOUT)/tests/none_tests.cpp			\
  $(STOUT)/tests/os_tests.cpp			\
  $(STOUT)/tests/strings_tests.cpp		\
  $(STOUT)/tests/thread_tests.cpp		\
  $(STOUT)/tests/uuid_tests.cpp

if OS_LINUX
  stout_tests_SOURCES += $(STOUT)/tests/proc_tests.cpp
endif

stout_tests_CPPFLAGS =				\
  -I$(srcdir)/$(STOUT)/include			\
  -I$(BOOST)					\
  -I$(GLOG)/src					\
  -I$(GTEST)/include				\
  -I$(GMOCK)/include				\
  -I$(PROTOBUF)/src				\
  $(AM_CPPFLAGS)

stout_tests_LDADD =			\
  libgmock.la				\
  $(GLOG)/libglog.la			\
  $(PROTOBUF)/src/libprotobuf.la

TESTS = stout-tests

# Dependencies for all-local.
ALL_LOCAL =				\
  $(STOUT)/Makefile			\
  $(BOOST)-stamp			\
  $(GLOG)/libglog.la			\
  $(LIBEV)/libev.la			\
  $(PROTOBUF)/src/libprotobuf.la	\
  $(PROTOBUF)/src/protoc

if HAS_GPERFTOOLS
 ALL_LOCAL += $(GPERFTOOLS)/libprofiler.la
endif

all-local: $(ALL_LOCAL)

clean-local:
	rm -r -f $(CLEAN_EXTRACTED)
	rm -f *-stamp
