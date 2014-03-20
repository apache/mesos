# Generated with autoscan, then modified appropriately.
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.61])
AC_INIT([libprocess], [0.0.1])

# Have autoconf setup some variables related to the system.
AC_CANONICAL_HOST
AC_CANONICAL_BUILD
AC_CANONICAL_TARGET

AC_LANG([C++])

AC_CONFIG_MACRO_DIR([m4])

# Initialize automake.
# -Wno-portability, since we require GNU Make for % patterns.
AM_INIT_AUTOMAKE([-Wall -Werror -Wno-portability foreign])

# Required for linking non-POSIX libs.
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])

# Initialize libtool (LT_OUTPUT builds ./libtool immediately, needed
# if we want to do tests with libtool during configuration).
LT_PREREQ([2.2])
LT_INIT
LT_LANG([C++])
LT_OUTPUT

# The default CFLAGS/CXXFLAGS from autoconf when using gcc usually
# includes "-O2". These really slow down compiling our tests, so we
# turn them off and enable them (where desired) directly in the
# Makefile. Note that this should not have an impact on users setting
# CFLAGS/CXXFLAGS directly at configure time, or when running make.
AS_IF([test "x${ac_cv_env_CFLAGS_set}" = "x"], [CFLAGS="-g"])
AS_IF([test "x${ac_cv_env_CXXFLAGS_set}" = "x"], [CXXFLAGS="-g"])

# Save the configure arguments so we can pass them to any third-party
# libraries that we might run configure on (see
# 3rdparty/Makefile.am). One downside of our strategy for shipping
# and building third-party libraries is that we can't expose options
# from nested third-party configure scripts.
CONFIGURE_ARGS="$ac_configure_args"
AC_SUBST(CONFIGURE_ARGS)

AC_CONFIG_SUBDIRS([3rdparty/stout])

AC_CONFIG_FILES([Makefile])
AC_CONFIG_FILES([3rdparty/Makefile])

AC_ARG_ENABLE([install],
              AS_HELP_STRING([--enable-install],
                             [install libprocess]),
              [AC_MSG_ERROR([libprocess can not currently be installed])])

AC_ARG_ENABLE([optimize],
              AS_HELP_STRING([--disable-optimize],
                             [don't try to compile with optimizations]),
              [], [enable_optimize=yes])

AC_ARG_ENABLE([perftools],
              AS_HELP_STRING([--enable-perftools],
                             [enable google perftools]),
              [gperftools=yes])

AC_ARG_WITH([zlib],
            AS_HELP_STRING([--without-zlib],
                           [disables zlib compression, which means the webui
                            will be far less responsive; not recommended]),
            [], [with_zlib=yes])

AC_ARG_WITH([cxx11],
            AS_HELP_STRING([--without-cxx11],
                           [builds libprocess without C++11 support (deprecated)]),
            [], [with_cxx11=yes])


# Do some OS specific setup.
case "${target_os}" in
  linux*)
    LIBS="$LIBS -lrt" # For clock_gettime() in stout/stopwatch.hpp.
    OS_NAME=linux # Used below for OS_LINUX.
    ;;
  *)
    ;;
esac


# Used for conditionally building source files (e.g., only want to
# build stout/tests/proc_tests.cpp on Linux).
AM_CONDITIONAL([OS_LINUX], [test "x$OS_NAME" = "xlinux"])


# Default to gcc toolchain (we rely on some atomic builtins for now,
# that are also present with clang).
AC_PROG_CXX([g++])
AC_PROG_CC([gcc])


# Check if we should try and enable optimizations.
if test "x$enable_optimize" = "xyes"; then
  # For now, we only turn on optimizations for gcc.
  if test "x$GCC" = "xyes"; then
    CXXFLAGS="$CXXFLAGS -g2 -O2"
  fi
fi


# Check if clang was provided instead.
AC_MSG_CHECKING([if compiling with clang])

AC_LANG_PUSH([C++])
AC_COMPILE_IFELSE(
[AC_LANG_PROGRAM([], [[
#ifndef __clang__
       not clang
#endif
]])],
[CLANG=yes], [CLANG=no])
AC_LANG_POP([C++])

AC_MSG_RESULT([$CLANG])
AC_SUBST([CLANG])

# Force enable C++11 if compiling with clang.
# Force disable C++11 if compiling with g++-4.7.
if test "x$CLANG" = "xyes"; then
  if test "x$with_cxx11" != "xyes"; then
    AC_MSG_WARN([Force enabling C++11 for Clang build])
  fi
  with_cxx11=yes
else
  # Check the version of gcc and add any flags as appropriate. Note
  # that '-dumpversion' works for clang as well but as of clang 3.3 it
  # reports version 4.2.1 (for gcc backwards compatibility).
  GCC_VERSION="`${CC} -dumpversion`"
  AC_MSG_NOTICE([GCC version: $GCC_VERSION])
  test $? = 0 || AC_MSG_ERROR([failed to determine version of gcc])

  # Check for GCC version 4.8.
  AX_COMPARE_VERSION([$GCC_VERSION], [eq2], [4.8],
                     [is_gxx48=yes], [is_gxx48=no])
  if test "x$is_gxx48" = "xyes"; then
    AC_MSG_NOTICE([Setting up CXXFLAGS for g++-4.8])
    # Boost 1.53.0 fails to compile with GCC 4.8 without
    # -Wno-unused-local-typedefs, and automake does not recognize the
    # flag.
    # TODO(brenden): Remove this when Boost has a resolution.
    CFLAGS="${CFLAGS} -Wno-unused-local-typedefs"
    CXXFLAGS="${CXXFLAGS} -Wno-unused-local-typedefs"
  fi

  # Check for GCC version == 4.7 and disable C++11. The C++11 version
  # of libprocess' Future uses features that 4.7 doesn't support. The
  # non-C++11 version of libprocess' Future uses std::tr1 code that is
  # only available without-cxx11.
  AX_COMPARE_VERSION([$GCC_VERSION], [eq2], [4.7],
                     [is_gxx47=yes], [is_gxx47=no])
  if test "x$is_gxx47" = "xyes"; then
    AC_MSG_WARN([Force disabling C++11 for g++-4.7 build])
    with_cxx11=no
  fi
fi


# Check for pthreads (uses m4/acx_pthread.m4).
ACX_PTHREAD([], [AC_MSG_ERROR([failed to find pthreads])])


AM_CONDITIONAL([HAS_GPERFTOOLS], [test "x$gperftools" = "xyes"])


# Check if we should/can build with libz.
if test "x$with_zlib" = "xyes"; then
  AC_CHECK_LIB([z], [deflate, gzread, gzwrite, inflate], [],
               [AC_MSG_ERROR([cannot find libz
  -------------------------------------------------------------------
  This means HTTP responses will be slower because we cannot use
  compression; you probably want to download and install zlib, but
  you can get away without it by doing --without-zlib.
  -------------------------------------------------------------------
  ])])
fi


# Check if we should/can build with C++11.
if test "x$with_cxx11" = "xyes"; then
  AX_CXX_COMPILE_STDCXX_11([noext], [mandatory])

  case "$host_os" in
  darwin* )
    # If we're using clang, we need to pass -stdlib=libc++ too.
    if test "x$CLANG" = "xyes"; then
      CXXFLAGS="$CXXFLAGS -stdlib=libc++"
    fi

    # GTEST on OSX needs its own tr1 tuple.
    # TODO(dhamon): Update to gmock 1.7 and pass GTEST_LANG_CXX11 when in
    # c++11 mode.
    CXXFLAGS="$CXXFLAGS -DGTEST_USE_OWN_TR1_TUPLE=1"
    ;;
  esac

  # Also pass the flags to 3rdparty libraries.
  CONFIGURE_ARGS="$CONFIGURE_ARGS CXXFLAGS='$CXXFLAGS'"
fi


AC_OUTPUT
