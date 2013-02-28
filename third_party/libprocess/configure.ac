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

# The default CFLAGS/CXXFLAGS from autoconf when using gcc is usually
# "-g -O2". These really slow down compiling our tests, so we turn
# them off and enable them (where desired) directly in the
# Makefile. Note that this should not have an impact on users setting
# CFLAGS/CXXFLAGS directly at configure time, or when running make.
AS_IF([test "x${ac_cv_env_CFLAGS_set}" = "x"], [CFLAGS=""])
AS_IF([test "x${ac_cv_env_CXXFLAGS_set}" = "x"], [CXXFLAGS=""])

# Save the configure arguments so we can pass them to any third_party
# libraries that we might run configure on (see
# third_party/Makefile.am). One downside of our strategy for shipping
# and building third_party libraries is that we can't expose options
# from nested third_party configure scripts.
CONFIGURE_ARGS="$ac_configure_args"
AC_SUBST(CONFIGURE_ARGS)

AC_CONFIG_SUBDIRS([third_party/stout])

AC_CONFIG_FILES([Makefile])
AC_CONFIG_FILES([third_party/Makefile])

AC_ARG_ENABLE([install],
              AS_HELP_STRING([--enable-install],
                             [install libprocess]),
              [AC_MSG_ERROR([libprocess can not currently be installed])])

AC_ARG_ENABLE([perftools],
              AS_HELP_STRING([--disable-perftools],
                             [disable google perftools]),
              [], [gperftools=yes])

AC_ARG_WITH([zlib],
            AS_HELP_STRING([--without-zlib],
                           [disables zlib compression, which means the webui
                            will be far less responsive; not recommended]),
            [], [with_zlib=yes])

# Checks for gcc toolchain (we rely on some atomic builtins for now).
AC_PROG_CXX([g++])
AC_PROG_CC([gcc])

# Check for pthreads (uses m4/acx_pthread.m4).
ACX_PTHREAD([], [AC_MSG_ERROR([failed to find pthreads])])

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

AM_CONDITIONAL([HAS_GPERFTOOLS], [test "x$gperftools" = "xyes"])

AC_OUTPUT
