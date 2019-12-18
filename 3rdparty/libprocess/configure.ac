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

# Generated with autoscan, then modified appropriately.
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.61])
AC_INIT([libprocess], [0.0.1])

# Have autoconf setup some variables related to the system.
AC_CANONICAL_HOST
AC_CANONICAL_BUILD
AC_CANONICAL_TARGET

AC_LANG([C++])

# Default to gcc toolchain (we rely on some atomic builtins for now,
# that are also present with clang).
AC_PROG_CXX([g++])
AC_PROG_CC([gcc])

AX_COMPILER_VERSION
AX_COMPILER_VENDOR

AC_CONFIG_MACRO_DIR([m4])

# Initialize automake.
# -Wno-portability, since we require GNU Make for % patterns.
# Removal of -Werror due warnings introduced by automake 1.14.
AM_INIT_AUTOMAKE([-Wall -Wno-portability foreign])
AM_MAINTAINER_MODE([enable])

# Required for linking non-POSIX libs.
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])

# Initialize libtool (LT_OUTPUT builds ./libtool immediately, needed
# if we want to do tests with libtool during configuration).
LT_PREREQ([2.2])
LT_INIT
LT_LANG([C++])
LT_OUTPUT

# Save the configure arguments so we can pass them to any third-party
# libraries that we might run configure on (see
# 3rdparty/Makefile.am). One downside of our strategy for shipping
# and building third-party libraries is that we can't expose options
# from nested third-party configure scripts.
CONFIGURE_ARGS="$ac_configure_args"
AC_SUBST(CONFIGURE_ARGS)

AM_CONDITIONAL([STANDALONE_LIBPROCESS], [true])

AC_CONFIG_FILES([Makefile])
AC_CONFIG_FILES([include/Makefile])
AC_CONFIG_FILES([3rdparty/Makefile])

AC_CONFIG_FILES([3rdparty/gmock_sources.cc])

###############################################################################
# Optional features.
###############################################################################

# TODO(benh): Eventually make this enabled by default.
AC_ARG_ENABLE([last_in_first_out_fixed_size_semaphore],
              AS_HELP_STRING([--enable-last-in-first-out-fixed-size-semaphore],
                             [enables the optimized LIFO fixed-size semaphore]),
                             [], [enable_last_in_first_out_fixed_size_semaphore=no])

# TODO(benh): Eventually make this enabled by default.
AC_ARG_ENABLE([lock_free_event_queue],
              AS_HELP_STRING([--enable-lock-free-event-queue],
                             [enables the lock-free event queue]),
                             [], [enable_lock_free_event_queue=no])

# TODO(benh): Eventually make this enabled by default.
AC_ARG_ENABLE([lock_free_run_queue],
              AS_HELP_STRING([--enable-lock-free-run-queue],
                             [enables the lock-free run queue]),
                             [], [enable_lock_free_run_queue=no])

AC_ARG_ENABLE([hardening],
              AS_HELP_STRING([--disable-hardening],
                             [disables security measures such as stack
                             protection and position independent library code]),
                             [], [enable_hardening=yes])

AC_ARG_ENABLE([bundled],
              AS_HELP_STRING([--disable-bundled],
                             [build against preinstalled dependencies instead
                             of bundled libraries]),
              [], [enable_bundled=yes])

AC_ARG_ENABLE([debug],
              AS_HELP_STRING([--enable-debug],
                             [enable debugging. If CFLAGS/CXXFLAGS are set, this
                              option won't change them default: no]),
              [], [enable_debug=no])

AC_ARG_ENABLE([install],
              AS_HELP_STRING([--enable-install],
                             [install libprocess]),
              [AC_MSG_ERROR([libprocess cannot currently be installed])])

AC_ARG_ENABLE([libevent],
              AS_HELP_STRING([--enable-libevent],
                             [use libevent instead of libev default: no]),
              [], [enable_libevent=no])

AC_ARG_ENABLE([optimize],
              AS_HELP_STRING([--enable-optimize],
                             [enable optimizations. If CFLAGS/CXXFLAGS are set,
                              this option won't change them default: no]),
              [], [enable_optimize=no])

AC_ARG_ENABLE([perftools],
              AS_HELP_STRING([--enable-perftools],
                             [enable Google perftools]),
              [], [enable_perftools=no])

AC_ARG_ENABLE([ssl],
              AS_HELP_STRING([--enable-ssl],
                             [use ssl for libprocess communication
                             default: no]),
              [], [enable_ssl=no])

AC_ARG_ENABLE([static-unimplemented],
              AS_HELP_STRING([--enable-static-unimplemented],
                             [generate static assertion errors for unimplemented
                             functions default: no]),
              [], [enable_static_unimplemented=no])


###############################################################################
# Optional packages.
###############################################################################

AC_ARG_WITH([apr],
            AS_HELP_STRING([--with-apr=@<:@=DIR@:>@],
                           [specify where to locate the apr-1 library]),
            [], [])

AC_ARG_WITH([boost],
            AS_HELP_STRING([--with-boost@<:@=DIR@:>@],
                           [excludes building and using the bundled Boost
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_boost=yes], [])

AC_ARG_WITH([concurrentqueue],
            AS_HELP_STRING([--with-concurrentqueue@<:@=DIR@:>@],
                           [excludes building and using the bundled concurrentqueue
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_concurrentqueue=yes], [])

AC_ARG_WITH([curl],
            AS_HELP_STRING([--with-curl=@<:@=DIR@:>@],
                           [specify where to locate the curl library]),
            [], [])

AC_ARG_WITH([elfio],
            AS_HELP_STRING([--with-elfio@<:@=DIR@:>@],
                           [excludes building and using the bundled ELFIO
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_elfio=yes], [])

AC_ARG_WITH([glog],
            AS_HELP_STRING([--with-glog@<:@=DIR@:>@],
                           [excludes building and using the bundled glog
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_glog=yes], [])

AC_ARG_WITH([gmock],
            AS_HELP_STRING([--with-gmock@<:@=DIR@:>@],
                           [excludes building and using the bundled gmock
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_gmock=yes], [])

AC_ARG_WITH([grpc],
            AS_HELP_STRING([--with-grpc@<:@=DIR@:>@],
                           [excludes building and using the bundled grpc
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_grpc=yes], [])

AC_ARG_WITH([http-parser],
            AS_HELP_STRING([--with-http-parser@<:@=DIR@:>@],
                           [excludes building and using the bundled http-parser
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_http_parser=yes], [])

AC_ARG_WITH([libev],
            AS_HELP_STRING([--with-libev@<:@=DIR@:>@],
                           [excludes building and using the bundled libev
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_libev=yes], [])

AC_ARG_WITH([libevent],
            AS_HELP_STRING([--with-libevent=@<:@=DIR@:>@],
                           [specify where to locate the libevent library]),
            [without_bundled_libevent=yes], [])

AC_ARG_WITH([picojson],
            AS_HELP_STRING([--with-picojson@<:@=DIR@:>@],
                           [excludes building and using the bundled picojson
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_picojson=yes], [])

AC_ARG_WITH([rapidjson],
            AS_HELP_STRING([--with-rapidjson@<:@=DIR@:>@],
                           [excludes building and using the bundled rapidjson
                           package in lieu of an installed version at a
                           location prefixed by the given path]),
            [without_bundled_rapidjson=yes], [])

AC_ARG_WITH([protobuf],
            AS_HELP_STRING([--with-protobuf@<:@=DIR@:>@],
                           [excludes building and using the bundled protobuf
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_protobuf=yes], [])

AC_ARG_WITH([ssl],
            AS_HELP_STRING([--with-ssl=@<:@=DIR@:>@],
                           [specify where to locate the ssl library]),
            [], [])

AC_ARG_WITH([stout],
            AS_HELP_STRING([--with-stout=@<:@=DIR@:>@],
                           [specify where to locate stout library]),
            [], [])

AC_ARG_WITH([svn],
            AS_HELP_STRING([--with-svn=@<:@=DIR@:>@],
                           [specify where to locate the svn-1 library]),
            [], [])

AC_ARG_WITH([zlib],
            AS_HELP_STRING([--with-zlib=@<:@=DIR@:>@],
                           [specify where to locate the zlib library]),
            [], [])


###############################################################################
# Miscellaneous flags/library/tool checks.
###############################################################################
AC_ARG_VAR([TEST_DRIVER], [executable and arguments of a test driver])

###############################################################################
# Compiler checks.
###############################################################################

# Check if we should use the LIFO fixed-size semaphore.
AS_IF([test "x$enable_last_in_first_out_fixed_size_semaphore" = "xyes"],
      [AC_DEFINE([LAST_IN_FIRST_OUT_FIXED_SIZE_SEMAPHORE])])

# Check if we should use the lock-free run queue.
AS_IF([test "x$enable_lock_free_event_queue" = "xyes"],
      [AC_DEFINE([LOCK_FREE_EVENT_QUEUE])])

# Check if we should use the lock-free run queue.
AS_IF([test "x$enable_lock_free_run_queue" = "xyes"],
      [AC_DEFINE([LOCK_FREE_RUN_QUEUE])])

# Check to see if we should harden or not.
AM_CONDITIONAL([ENABLE_HARDENING], [test x"$enable_hardening" = "xyes"])

AS_CASE($ax_cv_cxx_compiler_vendor,
  [clang], [
    # Upstream Clang 3.6 emits `-Wunused-local-typedef` warnings when compiling
    # Boost so we suppress them here. Apple LLVM based on Clang 3.6 doesn't have
    # the same behavior. See: https://reviews.apache.org/r/32749/
    # NOTE(chhsiao): Clang 3.6.0 from the link below seems to compile Boost
    # 1.53.0/1.65.0 just fine: http://releases.llvm.org/download.html#3.6.0
    AX_APPEND_COMPILE_FLAGS([-Wno-unused-local-typedef], [CXXFLAGS], [-Werror])
  ], [gnu], [
    # Check for GCC version >= 4.8.
    AX_COMPARE_VERSION([$ax_cv_cxx_compiler_version], [ge], [4.8],
                        [is_ge_gxx48=yes], [is_ge_gxx48=no])
    if test "x$is_ge_gxx48" != "xyes"; then
      # GCC < 4.8 is not supported.
      AC_MSG_ERROR([GCC 4.8 or higher required (found $ax_cv_cxx_compiler_version)])
    fi

    AC_MSG_NOTICE([Setting up CXXFLAGS for g++ version >= 4.8])
    # Boost fails to compile with GCC 4.8+ without `-Wno-unused-local-typedefs`,
    # and automake does not recognize the flag.
    # TODO(brenden): Remove this when Boost has a resolution.
    CFLAGS="${CFLAGS} -Wno-unused-local-typedefs"
    CXXFLAGS="${CXXFLAGS} -Wno-unused-local-typedefs"
  ], [
    AC_MSG_WARN([$ax_cv_cxx_compiler_vendor is an unsupported compiler])
  ]
)

# If our compiler supports strong stack protection, use it.
# If not, use a lesser form of stack protection.
AX_CHECK_COMPILE_FLAG([-fstack-protector-strong],
  [AC_SUBST(STACK_PROTECTOR, "-fstack-protector-strong")],
  [AC_SUBST(STACK_PROTECTOR, "-fstack-protector")])

# Ensure that the build environment supports C++11 (with "strict" conformance),
# and set "--std=" flag and CXXFLAGS environment variable as appropriate.
AX_CXX_COMPILE_STDCXX([11], [noext], [mandatory])

# Blacklist known incompatible C++ standard libraries to reduce chance
# of users accidentally using them.
#  - libstdc++ (Pre 4.8.0, version datestamp 20130322)
#  - libc++ (Pre 3.5.0, version string 1101)
AC_LANG_PUSH([C++])
AC_COMPILE_IFELSE(
[AC_LANG_SOURCE([[
#include <vector>

#ifdef __GLIBCXX__
#if __GLIBCXX__ < 20130322
#error Force the compiler to fail and set COMPATIBLE_CXX_STDLIB=no.
#endif
#elif _LIBCPP_VERSION < 1101
#error Force the compiler to fail and set COMPATIBLE_CXX_STDLIB=no.
#endif
]])],
[COMPATIBLE_CXX_STDLIB=yes], [COMPATIBLE_CXX_STDLIB=no])
AC_LANG_POP([C++])

if test "x$COMPATIBLE_CXX_STDLIB" = "xno"; then
  AC_MSG_ERROR([known incompatible C++ Standard library in use
-------------------------------------------------------------------
libstdc++ from GCC 4.8.0+ or libc++ from LLVM/Clang 3.5+ is required.

If a new enough GCC installed (4.8.0+) it should automatically find
the right libstdc++. If clang is in use, it will automatically find
the newest libstdc++ on the host.

If you are trying to use a different compiler from (clang, gcc) or a
alternate C++ Standard Library implementation, likely the flags being
passed to the compiler (CXXFLAGS) are not currently correct / being
picked up.
-------------------------------------------------------------------
])
fi

case "$host_os" in
  darwin* )
    # If we're using clang, we need to pass -stdlib=libc++ too.
    if test "x$ax_cv_cxx_compiler_vendor" = "xclang"; then
      CXXFLAGS="$CXXFLAGS -stdlib=libc++"
    fi
    ;;
  freebsd* )
    # If we're using clang, we need to pass -stdlib=libc++ too.
    if test "x$ax_cv_cxx_compiler_vendor" = "xclang"; then
      CXXFLAGS="$CXXFLAGS -stdlib=libc++"
    fi

    # FreeBSD needs GTEST_HAS_PTHREAD for this version of gmock/gtest and
    # _WITH_GETLINE for getline()
    CXXFLAGS="$CXXFLAGS -DGTEST_HAS_PTHREAD -D_WITH_GETLINE"
    ;;
esac


###############################################################################
# Miscellaneous flags/library/tool checks
###############################################################################

# Check for pthreads (uses m4/ax_pthread.m4).
AX_PTHREAD([], [AC_MSG_ERROR([failed to find pthreads])])

if test "x$enable_static_unimplemented" = "xyes"; then
  AC_DEFINE([ENABLE_STATIC_UNIMPLEMENTED], [1])
fi

AM_CONDITIONAL([HAS_GPERFTOOLS], [test x"$enable_perftools" = "xyes"])
AM_COND_IF([HAS_GPERFTOOLS], AC_DEFINE([ENABLE_GPERFTOOLS], [1]))

# Disable static libprocess by default.
AM_CONDITIONAL([ENABLE_STATIC_LIBPROCESS], [false])

# Do some OS specific setup.
case "${target_os}" in
  linux*)
    LIBS="$LIBS -lrt" # For clock_gettime() in stout/stopwatch.hpp.
    OS_NAME=linux # Used below for OS_LINUX.
    ;;
  darwin*)
    OS_NAME=darwin
    ;;
  freebsd*)
    OS_NAME=freebsd
    ;;
  *)
    ;;
esac

# Used for conditionally building source files (e.g., only want to
# build stout/tests/proc_tests.cpp on Linux).
AM_CONDITIONAL([OS_LINUX], [test "x$OS_NAME" = "xlinux"])
AM_CONDITIONAL([OS_FREEBSD], [test "x$OS_NAME" = "xfreebsd"])

if test "x$OS_NAME" != "xfreebsd"; then
    AC_CHECK_LIB([dl], [dlopen], [],
             [AC_MSG_ERROR([cannot find libdl
-------------------------------------------------------------------
libdl was not found, and is required for compilation.
-------------------------------------------------------------------
    ])])
fi

# Check if we should enable debugging, optimization. Note we only
# update CFLAGS and CXXFLAGS if none are provided.
AM_CONDITIONAL([DEBUG], [test x"$enable_debug" = "xyes"])
AM_CONDITIONAL([OPTIMIZE], [test x"$enable_optimize" = "xyes"])

debug_flags="-g1"
if test "x$enable_debug" = "xyes"; then
  debug_flags="-g -fno-omit-frame-pointer"
elif test "x$enable_optimize" = "xyes"; then
  debug_flags=""
fi

if test "x$enable_optimize" = "xyes"; then
  optimize_flags="-O2"
else
  optimize_flags="-O0"
fi

AS_IF([test "x${ac_cv_env_CFLAGS_set}" = "x"],
      [CFLAGS="$CFLAGS $debug_flags $optimize_flags"])
AS_IF([test "x${ac_cv_env_CXXFLAGS_set}" = "x"],
      [CXXFLAGS="$CXXFLAGS $debug_flags $optimize_flags"])

# Check if we need to do anything special to support using the fts_*
# functions. On some systems (e.g., those that use musl instead of
# glibc), FTS support requires linking against an additional library.
AC_CHECK_HEADERS([fts.h], [], [AC_MSG_ERROR([cannot find fts.h
-------------------------------------------------------------------
Failed to find fts.h. If this header file is not provided by libc
on this system (e.g., musl), a separate FTS package must be installed.
-------------------------------------------------------------------
])])

AC_SEARCH_LIBS(fts_close, fts, [], [AC_MSG_ERROR([cannot find libfts
-------------------------------------------------------------------
Failed to find fts_close. If this function is not provided by libc
on this system (e.g., musl), a separate FTS package must be installed.
-------------------------------------------------------------------
])])

###############################################################################
# Miscellaneous checks related to (un)bundled software.
###############################################################################

# On OS X, if `--with-apr` was not provided, we try to use an available
# brew installed version.
if test -z "`echo $with_apr`" &&
   test "$OS_NAME" = "darwin" &&
   test -n "`command -v brew`" &&
   test -n "`brew list --versions apr`"; then
  with_apr=`brew --prefix apr`/libexec
fi

# Check if libapr-1 prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include/apr-1 and /lib path
# suffixes. We include /include/apr-1 because we include <apr*>
# headers directly.
if test -n "`echo $with_apr`" ; then
  CPPFLAGS="-I${with_apr}/include/apr-1 -I${with_apr}/include/apr-1.0 $CPPFLAGS"
  LDFLAGS="-L${with_apr}/lib $LDFLAGS"
else
  CPPFLAGS="-I/usr/include/apr-1 -I/usr/include/apr-1.0 $CPPFLAGS"
fi

AC_CHECK_HEADERS([apr_pools.h],
                 [AC_CHECK_LIB([apr-1], [apr_initialize], [],
                               [AC_MSG_ERROR([cannot find libapr-1
-------------------------------------------------------------------
libapr-1 is required for libprocess to build.
-------------------------------------------------------------------
                        ])])], [AC_MSG_ERROR([cannot find libapr-1 headers
-------------------------------------------------------------------
libapr-1 is required for libprocess to build.
-------------------------------------------------------------------
])])


if test -n "`echo $with_boost`"; then
  CPPFLAGS="$CPPFLAGS -I${with_boost}/include"
fi

# Check if user has asked us to use a preinstalled Boost, or if they
# asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_boost" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([boost/version.hpp], [found_boost=yes])

  if test "x$found_boost" = "xyes"; then
    with_bundled_boost=no
  else
    AC_MSG_ERROR([cannot find Boost
-------------------------------------------------------------------
You have requested the use of a non-bundled Boost but no suitable
Boost could be found.

You may want specify the location of Boost by providing a prefix
path via --with-boost=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_boost=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_BOOST], [test "x$with_bundled_boost" = "xyes"])


if test -n "`echo $with_concurrentqueue`"; then
  CPPFLAGS="$CPPFLAGS -I${with_concurrentqueue}"
fi

# Check if user has asked us to use a preinstalled concurrentqueue,
# or if they asked us to ignore all bundled libraries while compiling
# and linking.
if test "x$without_bundled_concurrentqueue" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([concurrentqueue.h], [found_concurrentqueue=yes])

  if test "x$found_concurrentqueue" = "xyes"; then
    with_bundled_concurrentqueue=no
  else
    AC_MSG_ERROR([cannot find concurrentqueue
-------------------------------------------------------------------
You have requested the use of a non-bundled concurrentqueue but no
suitable concurrentqueue could be found.

You may want specify the location of concurrentqueue by providing a
prefix path via --with-concurrentqueue=DIR, or check that the path
you provided is correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_concurrentqueue=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_CONCURRENTQUEUE],
               [test "x$with_bundled_concurrentqueue" = "xyes"])


# Include directories used by package managers (e.g., MacPorts).
if test "x$enable_bundled" != "xyes" && \
   test "x$OS_NAME" = "xdarwin"; then
  CPPFLAGS="$CPPFLAGS -I/opt/local/include"
fi


if test -n "`echo $with_curl`" ; then
  CPPFLAGS="$CPPFLAGS -I${with_curl}/include"
  LDFLAGS="$LDFLAGS -L${with_curl}/lib"
fi

AC_CHECK_LIB([curl], [curl_global_init], [],
             [AC_MSG_ERROR([cannot find libcurl
-------------------------------------------------------------------
libcurl is required for libprocess to build.
-------------------------------------------------------------------
])])


# If the user has asked not to include the bundled ELFIO headers,
# check to see if the path to the ELFIO headers has been specified. If
# it has, make sure that the path is absolute. If everything is in
# order, add this path to the CPPFLAGS.
if test "x$without_bundled_elfio" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  if test -n "`echo $with_elfio`"; then
    if test "$with_elfio" = "${with_elfio#/}"; then
      AC_MSG_ERROR([The path passed to --with-elfio must be absolute.])
    fi
     CPPFLAGS="-I${with_elfio} $CPPFLAGS"
  fi

  AC_CHECK_HEADERS([elfio/elfio.hpp], [],
                   [AC_MSG_ERROR([Cannot find the ELFIO headers
-------------------------------------------------------------------
You have requested the use of a non-bundled ELFIO but no suitable
ELFIO headers could be found. Make sure these headers are either
installed on the system or the path passed via --with-elfio is correct.
-------------------------------------------------------------------
  ])])
else
  with_bundled_elfio=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_ELFIO], [test "x$with_bundled_elfio" = "xyes"])


if test -n "`echo $with_glog`"; then
  CPPFLAGS="$CPPFLAGS -I${with_glog}/include"
  LDFLAGS="$LDFLAGS -L${with_glog}/lib"
fi

# Check if user has asked us to use a preinstalled glog, or if they
# asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_glog" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([glog/logging.h],
                   [AC_LINK_IFELSE(
                        [AC_LANG_PROGRAM([#include <glog/logging.h>],[])],
                        [TEST_LIBS="$TEST_LIBS -lglog"] [found_glog=yes],
                        [AC_MSG_WARN([glog is not installed.])])])

  if test "x$found_glog" = "xyes"; then
    with_bundled_glog=no
  else
    AC_MSG_ERROR([cannot find glog
-------------------------------------------------------------------
You have requested the use of a non-bundled glog but no suitable
glog could be found.

You may want specify the location of glog by providing a prefix
path via --with-glog=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_glog=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_GLOG], [test "x$with_bundled_glog" = "xyes"])


GMOCKSRC="gmock-all.cc"
GTESTSRC="gtest-all.cc"

if test -n "`echo $with_gmock`"; then
  CPPFLAGS="$CPPFLAGS -I${with_gmock} -I${with_gmock}/include \
    -I${with_gmock}/src -I${with_gmock}/gtest -I${with_gmock}/gtest/include \
    -I${with_gmock}/gtest/src"

elif test "x$enable_bundled" != "xyes"; then
  CPPFLAGS="$CPPFLAGS -I/usr/src/gmock"
  # On system installations the deps are handled separately.
  # So insert a placeholder for substitution.
  GTESTSRC="stdio.h"
  check_gtest=yes
fi

# Check if user has asked us to use a preinstalled gmock, or if they
# asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_gmock" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADER($GMOCKSRC,
                  [found_gmock=yes],
                  [])

  if test "x$found_gmock" = "xyes"; then
    if test "x$check_gtest" = "xyes"; then
      # TODO(jieyu): Symbol _init is not availble for libs on OS X.
      AC_CHECK_LIB([gtest],
                   [_init],
                   [],
                   [AC_MSG_ERROR([gtest missing])])
    fi
    with_bundled_gmock=no
  else
    AC_MSG_ERROR([cannot find gmock
-------------------------------------------------------------------
You have requested the use of a non-bundled gmock but no suitable
gmock could be found.

You may want specify the location of gmock by providing a prefix
path via --with-gmock=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_gmock=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_GMOCK], [test "x$with_bundled_gmock" = "xyes"])
AM_CONDITIONAL([LINKGTEST], [test "check_gtest" = "xyes"])
AC_SUBST([GMOCKSRC])
AC_SUBST([GTESTSRC])


if test -n "`echo $with_http_parser`"; then
  CPPFLAGS="$CPPFLAGS -I${with_http_parser}/include"
  LDFLAGS="$LDFLAGS -L${with_http_parser}/lib"
fi

# Check if user has asked us to use a preinstalled http-parser, or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_http_parser" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([http_parser.h],
                   [AC_CHECK_LIB([http_parser],
                                 [http_parser_init],
                                 [found_http_parser=yes])])

  if test "x$found_http_parser" = "xyes"; then
    with_bundled_http_parser=no
  else
    AC_MSG_ERROR([cannot find http-parser
-------------------------------------------------------------------
You have requested the use of a non-bundled http-parser but no suitable
http-parser could be found.

You may want specify the location of http-parser by providing a prefix
path via --with-http-parser=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_http_parser=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_HTTP_PARSER],
               [test "x$with_bundled_http_parser" = "xyes"])


if test -n "`echo $with_libev`"; then
  CPPFLAGS="$CPPFLAGS -I${with_libev}/include"
  LDFLAGS="$LDFLAGS -L${with_libev}/lib"
fi

# Check if user has asked us to use a preinstalled libev, or if they
# asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_libev" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([ev.h], [found_libev=yes])

  if test "x$found_libev" = "xyes"; then
    with_bundled_libev=no
    AC_MSG_WARN([libev found must have been built with -DEV_CHILD_ENABLE=0])
  else
    AC_MSG_ERROR([cannot find libev
-------------------------------------------------------------------
You have requested the use of a non-bundled libev but no suitable
libev could be found.

You may want specify the location of libev by providing a prefix
path via --with-libev=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_libev=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_LIBEV], [test "x$with_bundled_libev" = "xyes"])


# Check if libevent prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes.
if test -z "`echo $with_libevent`" &&
   test "$OS_NAME" = "darwin" &&
   test -n "`command -v brew`" &&
   test -n "`brew list --versions libevent`"; then
  with_libevent=`brew --prefix libevent`
fi

if test -n "`echo $with_libevent`"; then
  CPPFLAGS="-I${with_libevent}/include $CPPFLAGS"
  LDFLAGS="-L${with_libevent}/lib $LDFLAGS"
fi

if test "x$enable_libevent" = "xyes"; then
  # Check if user has asked us to use a preinstalled libevent, or if they
  # asked us to ignore all bundled libraries while compiling and
  # linking.
  if test "x$without_bundled_libevent" = "xyes" || \
     test "x$enable_bundled" != "xyes"; then
     MESOS_HAVE_LIBEVENT()
  else
    with_bundled_libevent=yes
  fi

  AC_DEFINE([USE_LIBEVENT], [1])
fi

AM_CONDITIONAL([ENABLE_LIBEVENT],
               [test x"$enable_libevent" = "xyes"])
AM_CONDITIONAL([WITH_BUNDLED_LIBEVENT],
               [test "x$with_bundled_libevent" = "xyes"])


if test -n "`echo $with_picojson`"; then
  CPPFLAGS="$CPPFLAGS -I${with_picojson}/include"
fi

# Check if user has asked us to use a preinstalled picojson, or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_picojson" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([picojson.h], [found_picojson=yes])

  if test "x$found_picojson" = "xyes"; then
    with_bundled_picojson=no
  else
    AC_MSG_ERROR([cannot find picojson
-------------------------------------------------------------------
You have requested the use of a non-bundled picojson but no suitable
picojson could be found.

You may want specify the location of picojson by providing a prefix
path via --with-picojson=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_picojson=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_PICOJSON], [test "x$with_bundled_picojson" = "xyes"])


if test -n "`echo $with_rapidjson`"; then
  CPPFLAGS="$CPPFLAGS -I${with_rapidjson}/include"
fi

# Check if user has asked us to use a preinstalled rapidjson, or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_rapidjson" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([rapidjson/rapidjson.h], [found_rapidjson=yes])

  if test "x$found_rapidjson" = "xyes"; then
    with_bundled_rapidjson=no
  else
    AC_MSG_ERROR([cannot find rapidjson
-------------------------------------------------------------------
You have requested the use of a non-bundled rapidjson but no suitable
rapidjson could be found.

You may want specify the location of rapidjson by providing a prefix
path via --with-rapidjson=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_rapidjson=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_RAPIDJSON], [test "x$with_bundled_rapidjson" = "xyes"])


if test -n "`echo $with_protobuf`"; then
  PROTOBUF_CPPFLAGS="-I${with_protobuf}/include"
  PROTOBUF_LINKERFLAGS="-L${with_protobuf}/lib"

  CPPFLAGS="$PROTOBUF_CPPFLAGS $CPPFLAGS"
  LDFLAGS="$PROTOBUF_LINKERFLAGS $LDFLAGS"
fi

# Check if user has asked us to use a preinstalled protobuf, or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_protobuf" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Use the system default prefix if not specified.
  if test -n "`echo $with_protobuf`"; then
    PROTOBUFPREFIX="${with_protobuf}"
  elif test "x$enable_bundled" != "xyes"; then
    PROTOBUFPREFIX="/usr"
  fi

  AC_CHECK_HEADERS([google/protobuf/message.h], [found_protobuf=yes],
                   [found_protobuf=no])

  if test "x$found_protobuf" = "xyes"; then
    saved_LIBS="$LIBS"
    LIBS="-lprotobuf $LIBS"
    AC_LANG_PUSH([C++])
    AC_LINK_IFELSE([AC_LANG_PROGRAM([#include <google/protobuf/stubs/common.h>],
                    [ google::protobuf::ShutdownProtobufLibrary(); ])],
            [ found_protobuf=yes ],
            [ found_protobuf=no ])
    AC_LANG_POP()
    LIBS="$saved_LIBS"
  fi

  if test "x$found_protobuf" = "xyes"; then
    with_bundled_protobuf=no

    AC_CHECK_TOOL([PROTOCOMPILER_TEST], [protoc], [], [$PROTOBUFPREFIX/bin])

    if test -z "`echo $PROTOCOMPILER_TEST`"; then
      AC_MSG_ERROR([protoc not found in PATH])
    fi

    PROTOCOMPILER="$PROTOBUFPREFIX/bin/protoc"

  else
    AC_MSG_ERROR([cannot find protobuf
-------------------------------------------------------------------
You have requested the use of a non-bundled protobuf but no suitable
protobuf could be found.

You may want specify the location of protobuf by providing a prefix
path via --with-protobuf=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_protobuf=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_PROTOBUF],
               [test "x$with_bundled_protobuf" = "xyes"])

AC_SUBST([PROTOBUF_CPPFLAGS])
AC_SUBST([PROTOBUF_LINKERFLAGS])
AC_SUBST([PROTOCOMPILER])


# Check if libssl prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes.
if test -z "`echo $with_ssl`" &&
   test "$OS_NAME" = "darwin" &&
   test -n "`command -v brew`" &&
   test -n "`brew list --versions openssl`"; then
  with_ssl=`brew --prefix openssl`
fi

if test -n "`echo $with_ssl`"; then
  SSL_CPPFLAGS="-I${with_ssl}/include"
  SSL_LINKERFLAGS="-L${with_ssl}/lib"

  CPPFLAGS="$SSL_CPPFLAGS $CPPFLAGS"
  LDFLAGS="$SSL_LINKERFLAGS $LDFLAGS"
fi

AC_CHECK_LIB([crypto], [RAND_poll], [found_crypto=yes])

AC_CHECK_HEADERS([openssl/ssl.h],
                 [AC_CHECK_LIB([ssl],
                               [SSL_CTX_new],
                               [found_ssl=yes],
                               [],
                               [-lcrypto])])

if test "x$enable_ssl" = "xyes"; then
  if test "x$found_ssl" != "xyes"; then
    AC_MSG_ERROR([cannot find libssl
-------------------------------------------------------------------
libssl is required for an SSL-enabled build.
-------------------------------------------------------------------
    ])
  fi

  if test "x$found_crypto" != "xyes"; then
    AC_MSG_ERROR([cannot find libcrypto
-------------------------------------------------------------------
libcrypto is required for an SSL-enabled build.
-------------------------------------------------------------------
    ])
  fi

  LIBS="-lssl -lcrypto $LIBS"

  if test "x$enable_libevent" = "xyes"; then
    if test "x$with_bundled_libevent" != "xyes"; then
      MESOS_HAVE_LIBEVENT_SSL()
    fi
  fi
  AC_DEFINE([USE_SSL_SOCKET], [1])
fi

AM_CONDITIONAL([ENABLE_SSL],
               [test x"$enable_ssl" = "xyes"])

AC_SUBST([SSL_CPPFLAGS])
AC_SUBST([SSL_LINKERFLAGS])


if test -n "`echo $with_stout`"; then
  CPPFLAGS="$CPPFLAGS -I${with_stout}/include"
fi

# Check if user has asked us to use a preinstalled stout, or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_stout" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([stout/try.hpp], [found_stout=yes])

  if test "x$found_stout" = "xyes"; then
    with_bundled_stout=no
  else
    AC_MSG_ERROR([cannot find stout
-------------------------------------------------------------------
You have requested the use of a non-bundled stout but no suitable
stout could be found.

You may want specify the location of stout by providing a prefix
path via --with-stout=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_stout=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_STOUT], [test "x$with_bundled_stout" = "xyes"])


# Check if libsvn-1 prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes. We include /include/subversion-1 because we include
# <svn_*> directly.
if test -z "`echo $with_svn`" &&
   test "$OS_NAME" = "darwin" &&
   test -n "`command -v brew`" &&
   test -n "`brew list --versions subversion`"; then
  with_svn=`brew --prefix subversion`
fi

if test -n "`echo $with_svn`"; then
  CPPFLAGS="-I${with_svn}/include/subversion-1 $CPPFLAGS"
  LDFLAGS="-L${with_svn}/lib $LDFLAGS"
else
  CPPFLAGS="-I/usr/include/subversion-1 $CPPFLAGS"
fi

AC_CHECK_HEADERS([svn_delta.h],
                 [AC_CHECK_LIB([svn_delta-1], [svn_txdelta], [],
                               [AC_MSG_ERROR([cannot find libsvn_delta-1
-------------------------------------------------------------------
libsubversion-1 is required for libprocess to build.
-------------------------------------------------------------------
                        ])])], [AC_MSG_ERROR([cannot find libsvn_delta-1 headers
-------------------------------------------------------------------
libsubversion-1 is required for libprocess to build.
-------------------------------------------------------------------
])])

AC_CHECK_HEADERS([svn_version.h],
                 [AC_CHECK_LIB([svn_subr-1], [svn_stringbuf_create_ensure], [],
                               [AC_MSG_ERROR([cannot find libsvn_subr-1
-------------------------------------------------------------------
libsubversion-1 is required for libprocess to build.
-------------------------------------------------------------------
                        ])])], [AC_MSG_ERROR([cannot find libsvn_subr-1 headers
-------------------------------------------------------------------
libsubversion-1 is required for libprocess to build.
-------------------------------------------------------------------
])])


# Check if zlib prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes.
if test -n "`echo $with_zlib`" ; then
  ZLIB_CPPFLAGS="-I${with_zlib}/include"
  ZLIB_LINKERFLAGS="-L${with_zlib}/lib"

  CPPFLAGS="$ZLIB_CPPFLAGS $CPPFLAGS"
  LDFLAGS="$ZLIB_LINKERFLAGS $LDFLAGS"
fi

AC_CHECK_HEADERS([zlib.h],
                 [AC_CHECK_LIB([z], [deflate, gzread, gzwrite, inflate], [],
                               [AC_MSG_ERROR([cannot find libz
-------------------------------------------------------------------
libz is required for libprocess to build.
-------------------------------------------------------------------
                               ])])],
                 [AC_MSG_ERROR([cannot find libz headers
-------------------------------------------------------------------
libz headers are required for libprocess to build.
-------------------------------------------------------------------
                 ])])

AC_SUBST([ZLIB_CPPFLAGS])
AC_SUBST([ZLIB_LINKERFLAGS])


# Check if grpc prefix path was supplied and if so, add it to the
# CPPFLAGS and LDFLAGS with respective /include and /lib path suffixes.
if test -n "`echo $with_grpc`"; then
  if test "x$without_bundled_protobuf" != "xyes"; then
    AC_MSG_ERROR([protobuf not specified
-------------------------------------------------------------------
You have requested the use of a non-bundled grpc without specifying
the protobuf library it uses. This is prohibited to avoid using
conflicting versions of protobuf libraries. Please specify the
location of protobuf used by grpc by providing a prefix path via
--with-protobuf=DIR.
-------------------------------------------------------------------
    ])
  fi

  CPPFLAGS="$CPPFLAGS -I${with_grpc}/include"
  LDFLAGS="$LDFLAGS -L${with_grpc}/lib"
fi

# Check if user has asked us to use a preinstalled grpc, or if they
# asked us to ignore all bundled libraries while compiling and linking.
if test "x$without_bundled_grpc" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Use the system default prefix if not specified.
  if test -n "`echo $with_grpc`"; then
    GRPCPREFIX="${with_grpc}"
  elif test "x$enable_bundled" != "xyes"; then
    GRPCPREFIX="/usr"
  fi

  if test "x$enable_ssl" = "xno"; then
    GRPC_VARIANT="_unsecure"
  fi

  # Check if headers and library were located.
  saved_LIBS="$LIBS"
  LIBS="-lgrpc++$GRPC_VARIANT -lgrpc$GRPC_VARIANT -lgpr $LIBS"
  AC_LANG_PUSH([C++])
  AC_CHECK_HEADERS([grpcpp/grpcpp.h],
                   [AC_LINK_IFELSE(
                       [AC_LANG_PROGRAM([#include <grpcpp/grpcpp.h>],
                                        [grpc::Version();])],
                       [found_grpc=yes],
                       [found_grpc=no])],
                   [found_grpc=no])
  AC_LANG_POP()
  LIBS="$saved_LIBS"

  if test "x$found_grpc" = "xyes"; then
    with_bundled_grpc=no

    AC_CHECK_TOOL([GRPC_CXX_PLUGIN_TEST],
                  [grpc_cpp_plugin],
                  [],
                  [$GRPCPREFIX/bin])

    if test -z "`echo $GRPC_CXX_PLUGIN_TEST`"; then
      AC_MSG_ERROR([grpc_cpp_plugin not found in PATH])
    fi

    GRPC_CXX_PLUGIN="$GRPCPREFIX/bin/grpc_cpp_plugin"

  else
    AC_MSG_ERROR([cannot find grpc
-------------------------------------------------------------------
You have requested the use of a non-bundled grpc but no suitable
grpc could be found.

You may want specify the location of grpc by providing a prefix
path via --with-grpc=DIR, or check that the path you provided is
correct if you are already doing this.
-------------------------------------------------------------------
    ])
  fi
else
  with_bundled_grpc=yes
fi

AM_CONDITIONAL([WITH_BUNDLED_GRPC],
               [test "x$with_bundled_grpc" = "xyes"])

AC_SUBST([GRPC_CXX_PLUGIN])


# Once all CXXFLAGS are completely assembled, make sure the we are not
# affected by libcxx undefined behavior,
# https://llvm.org/bugs/show_bug.cgi?id=28469.
AC_MSG_CHECKING([C++ standard library for undefined behaviour with selected optimization level])
# NOTE: We clear `LIBS` here to prevent linking in libraries unrelated
# to the test. These libraries might not be in the linker lookup paths.
saved_LIBS="$LIBS"
LIBS=""
AC_LANG_PUSH([C++])
AC_RUN_IFELSE([
  AC_LANG_SOURCE([[
    #include <functional>
    #include <map>

    struct F {
      std::map<int, std::function<void()>> m;
      F() { m[42] = []() {}; }
    };

    int main() {
      F f;
      f = F{};
    }
  ]])],
  [
    AC_MSG_RESULT([no])
  ], [
    AC_MSG_RESULT([yes])
    AC_MSG_ERROR([libprocess cannot be built with optimizations against this version of libcxx (MESOS-5745).
                  Consider building without optimizations, or changing the used C++ standard library.])
  ]
)
AC_LANG_POP([C++])
LIBS="$saved_LIBS"


# Also pass the flags to 3rdparty libraries.
CONFIGURE_ARGS="$CONFIGURE_ARGS CXXFLAGS='$CXXFLAGS'"


AC_OUTPUT
