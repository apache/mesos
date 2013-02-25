# This Makefile is for building third_party packages from
# tarballs. For autotools-based packages, we configure each of the
# packages to build static PIC binaries which we can safely link into
# a shared libmesos, and build it in-place without installing it (even
# if one runs 'make install' in this directory). Non-autotools based
# packages may be special cases; this Makefile is responsible for
# passing any special make or configure flags that might be required.

BUILT_SOURCES =

# We need to add '--srcdir=.' needed because 'make distcheck' adds
#  '--srcdir=...' when configuring.
CONFIGURE_ARGS = @CONFIGURE_ARGS@ --enable-shared=no --with-pic --srcdir=.

include versions.am

BOOST = boost-$(BOOST_VERSION)
GLOG = glog-$(GLOG_VERSION)
GMOCK = gmock-$(GMOCK_VERSION)
GPERFTOOLS = gperftools-$(GPERFTOOLS_VERSION)
GTEST = $(GMOCK)/gtest
RY_HTTP_PARSER = ry-http-parser-$(RY_HTTP_PARSER_VERSION)
LIBEV = libev-$(LIBEV_VERSION)


EXTRA_DIST = $(BOOST).tar.gz $(GLOG).tar.gz $(GMOCK).tar.gz		\
	$(GPERFTOOLS).tar.gz $(RY_HTTP_PARSER).tar.gz $(LIBEV).tar.gz	\
	libev-3.8.patch

CLEAN_EXTRACTED = $(BOOST) $(GLOG) $(GMOCK) $(GPERFTOOLS)		\
	$(RY_HTTP_PARSER) $(LIBEV)


# Convenience library for Ryan Dahl's HTTP parser.
noinst_LTLIBRARIES = libry_http_parser.la
nodist_libry_http_parser_la_SOURCES = $(RY_HTTP_PARSER)/http_parser.c
libry_http_parser_la_CPPFLAGS = -I$(RY_HTTP_PARSER)

# We list the sources in BUILT_SOURCES to make sure that the package
# gets unarchived first.
BUILT_SOURCES += $(nodist_libry_http_parser_la_SOURCES)


# Convenience library for gmock/gtest.
check_LTLIBRARIES = libgmock.la
nodist_libgmock_la_SOURCES = $(GTEST)/src/gtest-all.cc \
                             $(GMOCK)/src/gmock-all.cc
libgmock_la_CPPFLAGS = -I$(GTEST)/include -I$(GTEST) \
                       -I$(GMOCK)/include -I$(GMOCK)

# We list the sources in BUILT_SOURCES to make sure that the package
# gets unarchived first.
BUILT_SOURCES += $(nodist_libgmock_la_SOURCES)


# This is where the magic happens: we use stamp files as dependencies
# which cause the packages to get extracted as necessary. We also
# apply any patches as appropriate.
%-stamp:: %.tar.gz
	gzip -d -c $^ | tar xf -
	test ! -e $(srcdir)/$*.patch || patch -d $* -p1 <$(srcdir)/$*.patch
	touch $@


$(BOOST)/boost: $(BOOST)-stamp

$(GLOG)/libglog.la: $(GLOG)-stamp
	cd $(GLOG) && ./configure $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)

$(GMOCK)/src/gmock-all.cc: $(GMOCK)-stamp

$(GPERFTOOLS)/libprofiler.la: $(GPERFTOOLS)-build-stamp

$(GPERFTOOLS)-build-stamp: $(GPERFTOOLS)-stamp
	cd $(GPERFTOOLS) && ./configure  $(CONFIGURE_ARGS) && \
	  $(MAKE) $(AM_MAKEFLAGS)
	touch $@

$(GTEST)/src/gtest-all.cc: $(GMOCK)-stamp

$(RY_HTTP_PARSER)/http_parser.c: $(RY_HTTP_PARSER)-stamp

$(LIBEV)/libev.la: $(LIBEV)-stamp
	cd $(LIBEV) && ./configure $(CONFIGURE_ARGS) && \
          $(MAKE) $(AM_MAKEFLAGS)

THIRD_PARTY_LIBS = $(BOOST)-stamp $(GLOG)/libglog.la	\
	$(GPERFTOOLS)/libprofiler.la $(LIBEV)/libev.la

all-local: $(THIRD_PARTY_LIBS)

clean-local:
	rm -r -f $(CLEAN_EXTRACTED)
	rm -f *-stamp
