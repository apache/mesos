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

AC_CONFIG_FILES([Makefile])
AC_CONFIG_SUBDIRS([third_party/glog-0.3.1])
AC_CONFIG_SUBDIRS([third_party/libev-3.8])

# Temporary fix so we don't build dylib's on Mac OS X.
ac_configure_args="$ac_configure_args --enable-shared=no"

AC_OUTPUT
