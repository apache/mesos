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
AM_INIT_AUTOMAKE([-Wall -Werror foreign])

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

# Checks for gcc toolchain (we rely on some atomic builtins for now).
AC_PROG_CXX([g++])
AC_PROG_CC([gcc])

# Check for pthreads (uses m4/acx_pthread.m4).
ACX_PTHREAD([], [AC_MSG_ERROR([failed to find pthreads])])

AC_CONFIG_FILES([Makefile])
AC_CONFIG_FILES([third_party/Makefile])

AC_OUTPUT
