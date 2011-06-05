# Generated with autoscan, then modified appropriately.
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT
AC_CONFIG_SRCDIR([process.cpp])

# Checks for programs.
AC_PROG_CXX([g++])
AC_PROG_CC([gcc])

# Check for pthread library.
AC_CHECK_LIB([pthread], [pthread_create], [], [AC_MSG_FAILURE([
*** The pthread library is missing or cannot be found.])])

# Check for the ht.
AC_ARG_WITH([ht],
            AC_HELP_STRING([--with-ht=prefix],
                           [find headers in prefix/include/ht; library in prefix/lib]),
	    [
	    case "$with_ht" in
		 yes) AC_MSG_ERROR([
*** --with-ht requires an argument]) ;;
		 ''|no) ;;
		 *) CFLAGS="$CFLAGS -I$withval/include"
		    CPPFLAGS="$CPPFLAGS -I$withval/include"
		    CXXFLAGS="$CXXFLAGS -I$withval/include"
		    LDFLAGS="$LDFLAGS -L$withval/lib"
		    LIBS="-lht $LIBS" ;;
	    esac
	    ],
	    [])

AC_MSG_CHECKING(for ht)
AC_LINK_IFELSE([AC_LANG_PROGRAM([
		[#include <ht/ht.h>
                void entry() { }]],
		[ht_request(1);])],
		[AC_DEFINE([HAVE_HT], [1],
                          [Define if you have libht])
                 AC_MSG_RESULT(yes)],
		[AC_MSG_RESULT(no)])


# Check for the Lithe.
AC_ARG_WITH([lithe],
            AC_HELP_STRING([--with-lithe=prefix],
                           [find headers in prefix/include, library in prefix/lib]),
	    [
	    case "$with_lithe" in
		 yes) AC_MSG_ERROR([
*** --with-lithe requires an argument]) ;;
		 ''|no) ;;
		 *) CFLAGS="$CFLAGS -I$withval/include"
		    CPPFLAGS="$CPPFLAGS -I$withval/include"
		    CXXFLAGS="$CXXFLAGS -I$withval/include"
		    LDFLAGS="$LDFLAGS -L$withval/lib"
		    LIBS="-llithe $LIBS" ;;
	    esac
	    ],
	    [])

AC_MSG_CHECKING(for lithe)
AC_LINK_IFELSE([AC_LANG_PROGRAM([
		[#include <lithe.h>
                ]],
		[lithe_sched_request(1);])],
		[AC_DEFINE([HAVE_LITHE], [1],
                          [Define if you have Lithe])
                 AC_MSG_RESULT(yes)],
		[AC_MSG_RESULT(no)])

#AC_ARG_WITH([boost-headers],
#  AC_HELP_STRING([--with-boost-headers=DIR],
#                 [find Boost headers in DIR]),
#	         [case "$with_boost_headers" in
#		    yes) AC_MSG_ERROR([
#*** --with-boost-headers requires an argument]) ;;
#		    ''|no) AC_MSG_ERROR([
#*** The Boost headers are REQUIRED and cannot
#*** be ommited using --without-boost-headers.]) ;;
#		    *) CPPFLAGS="$CPPFLAGS -I$withval"
#                       CFLAGS="$CFLAGS -I$withval"
#                       CXXFLAGS="$CXXFLAGS -I$withval" ;;
#	          esac],
#	         [])

# Look for BOOST_PP.
#AC_LANG_PUSH([C++])
#AC_CHECK_HEADERS([boost/preprocessor/library.hpp], [], [AC_MSG_FAILURE([
#*** The BOOST_PP macros are missing or cannot be found.
#*** Did you try using --with-boost-headers=DIR?])], [])
#AC_LANG_POP([C++])

# Check for BOOST_FOREACH.
#AC_LANG_PUSH([C++])
#AC_CHECK_HEADERS([boost/foreach.hpp], [], [AC_MSG_FAILURE([
#*** The BOOST_FOREACH macro is missing or cannot be found.
#*** Did you try using --with-boost-headers=DIR?])], [])
#AC_LANG_POP([C++])

# Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS([arpa/inet.h fcntl.h limits.h netdb.h netinet/in.h stdlib.h string.h sys/ioctl.h sys/socket.h sys/time.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_C_CONST
AC_C_INLINE
AC_TYPE_SIZE_T
AC_HEADER_TIME

# Checks for library functions.
AC_PROG_GCC_TRADITIONAL
AC_FUNC_MALLOC
AC_FUNC_MMAP
AC_FUNC_REALLOC
AC_FUNC_SELECT_ARGTYPES
AC_CHECK_FUNCS([gethostbyname gethostname getpagesize memset select socket])

AC_CONFIG_FILES([Makefile examples/Makefile swig/Makefile])
AC_CONFIG_SUBDIRS([third_party/libev-3.8])

# Temporary fix so we don't build dylib's on Mac OS X.
ac_configure_args="$ac_configure_args --enable-shared=no"

AC_OUTPUT
