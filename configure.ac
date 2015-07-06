# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Generated with autoscan, then modified appropriately.
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.61])
AC_INIT([mesos], [0.24.0])

# Have autoconf setup some variables related to the system.
AC_CANONICAL_HOST
AC_CANONICAL_BUILD
AC_CANONICAL_TARGET
AC_DISABLE_STATIC

AC_LANG([C++])

AC_CONFIG_MACRO_DIR([m4])

# Initialize automake.
# -Wno-portability, since we require GNU Make for % patterns
# Removal of -Werror due warnings introduced by automake 1.14.
AM_INIT_AUTOMAKE([-Wall -Wno-portability foreign])

# This is required for linking non-POSIX libs.
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])

# Initialize libtool (LT_OUTPUT builds ./libtool immediately, needed
# if we want to do tests with libtool during configuration).
LT_PREREQ([2.2])
LT_INIT
LT_LANG([C++])
LT_OUTPUT

# Save the configure arguments so we can pass them to any third-party
# libraries that we might run configure on (see 3rdparty/Makefile.am).
# One downside of our strategy for shipping and building third-party
# libraries is that we can't expose options from nested third-party
# configure scripts.
CONFIGURE_ARGS="$ac_configure_args"
AC_SUBST(CONFIGURE_ARGS)

# Force configured third-party libraries (currently only libprocess)
# to only build a static library with position independent code so
# that we can produce a final shared library which includes everything
# necessary (and only install that).
ac_configure_args_pre="$ac_configure_args"
ac_configure_args_post="$ac_configure_args --enable-shared=no --with-pic"
ac_configure_args="$ac_configure_args_post"

# Make sure config.status doesn't get the changed configure arguments,
# so it can rerun configure in the root directory correctly. This is
# necessary for Makefile rules which would regenerate files (e.g.,
# 'Makefile') after configure.ac was updated.
AC_CONFIG_COMMANDS_PRE([ac_configure_args="$ac_configure_args_pre"])
AC_CONFIG_COMMANDS_POST([ac_configure_args="$ac_configure_args_post"])

AC_CONFIG_SUBDIRS([3rdparty/libprocess])

AC_CONFIG_FILES([Makefile mesos.pc])
AC_CONFIG_FILES([ec2/Makefile])
AC_CONFIG_FILES([src/Makefile])
AC_CONFIG_FILES([3rdparty/Makefile])

AC_CONFIG_FILES([bin/mesos.sh], [chmod +x bin/mesos.sh])
AC_CONFIG_FILES([bin/mesos-local.sh], [chmod +x bin/mesos-local.sh])
AC_CONFIG_FILES([bin/mesos-master.sh], [chmod +x bin/mesos-master.sh])
AC_CONFIG_FILES([bin/mesos-slave.sh], [chmod +x bin/mesos-slave.sh])
AC_CONFIG_FILES([bin/mesos-tests.sh], [chmod +x bin/mesos-tests.sh])
AC_CONFIG_FILES([bin/mesos-local-flags.sh])
AC_CONFIG_FILES([bin/mesos-master-flags.sh])
AC_CONFIG_FILES([bin/mesos-slave-flags.sh])
AC_CONFIG_FILES([bin/mesos-tests-flags.sh])
AC_CONFIG_FILES([bin/gdb-mesos-local.sh], [chmod +x bin/gdb-mesos-local.sh])
AC_CONFIG_FILES([bin/gdb-mesos-master.sh], [chmod +x bin/gdb-mesos-master.sh])
AC_CONFIG_FILES([bin/gdb-mesos-slave.sh], [chmod +x bin/gdb-mesos-slave.sh])
AC_CONFIG_FILES([bin/gdb-mesos-tests.sh], [chmod +x bin/gdb-mesos-tests.sh])
AC_CONFIG_FILES([bin/lldb-mesos-local.sh], [chmod +x bin/lldb-mesos-local.sh])
AC_CONFIG_FILES([bin/lldb-mesos-master.sh],
		[chmod +x bin/lldb-mesos-master.sh])
AC_CONFIG_FILES([bin/lldb-mesos-slave.sh], [chmod +x bin/lldb-mesos-slave.sh])
AC_CONFIG_FILES([bin/lldb-mesos-tests.sh], [chmod +x bin/lldb-mesos-tests.sh])
AC_CONFIG_FILES([bin/valgrind-mesos-local.sh],
		[chmod +x bin/valgrind-mesos-local.sh])
AC_CONFIG_FILES([bin/valgrind-mesos-slave.sh],
		[chmod +x bin/valgrind-mesos-slave.sh])
AC_CONFIG_FILES([bin/valgrind-mesos-master.sh],
		[chmod +x bin/valgrind-mesos-master.sh])
AC_CONFIG_FILES([bin/valgrind-mesos-tests.sh],
		[chmod +x bin/valgrind-mesos-tests.sh])
AC_CONFIG_FILES([src/deploy/mesos-daemon.sh])
AC_CONFIG_FILES([src/deploy/mesos-start-cluster.sh])
AC_CONFIG_FILES([src/deploy/mesos-start-masters.sh])
AC_CONFIG_FILES([src/deploy/mesos-start-slaves.sh])
AC_CONFIG_FILES([src/deploy/mesos-stop-cluster.sh])
AC_CONFIG_FILES([src/deploy/mesos-stop-masters.sh])
AC_CONFIG_FILES([src/deploy/mesos-stop-slaves.sh])

AC_CONFIG_FILES([include/mesos/version.hpp])

AC_CONFIG_FILES([src/java/generated/org/apache/mesos/MesosNativeLibrary.java])

AC_CONFIG_FILES([mpi/mpiexec-mesos], [chmod +x mpi/mpiexec-mesos])


AC_ARG_ENABLE([java],
              AS_HELP_STRING([--disable-java],
                             [don't build Java bindings]),
              [], [enable_java=yes])

AC_ARG_ENABLE([python],
              AS_HELP_STRING([--disable-python],
                             [don't build Python bindings]),
              [], [enable_python=yes])

AC_ARG_ENABLE([debug],
              AS_HELP_STRING([--enable-debug],
                             [enable debugging. If CFLAGS/CXXFLAGS are set, this
                              option won't change them default: no]),
              [enable_debug=yes], [])

AC_ARG_ENABLE([optimize],
              AS_HELP_STRING([--enable-optimize],
                             [enable optimizations. If CFLAGS/CXXFLAGS are set,
                              this option won't change them default: no]),
              [enable_optimize=yes], [])

AC_ARG_ENABLE([bundled],
              AS_HELP_STRING([--disable-bundled],
                             [build against preinstalled dependencies instead
                              of bundled libraries]),
              [enable_bundled=no], [enable_bundled=yes])

AC_ARG_WITH([zookeeper],
            AS_HELP_STRING([--with-zookeeper@<:@=DIR@:>@],
                           [excludes building and using the bundled ZooKeeper
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_zookeeper=yes], [])

AC_ARG_WITH([leveldb],
            AS_HELP_STRING([--with-leveldb@<:@=DIR@:>@],
                           [excludes building and using the bundled LevelDB
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_leveldb=yes], [])

AC_ARG_WITH([glog],
            AS_HELP_STRING([--with-glog@<:@=DIR@:>@],
                           [excludes building and using the bundled glog
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_glog=yes], [])

AC_ARG_WITH([protobuf],
            AS_HELP_STRING([--with-protobuf@<:@=DIR@:>@],
                           [excludes building and using the bundled protobuf
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_protobuf=yes], [])

AC_ARG_WITH([gmock],
            AS_HELP_STRING([--with-gmock@<:@=DIR@:>@],
                           [excludes building and using the bundled gmock
                            package in lieu of an installed version at a
                            location prefixed by the given path]),
            [without_bundled_gmock=yes], [])

AC_ARG_WITH([curl],
            AS_HELP_STRING([--with-curl=@<:@=DIR@:>@],
                           [specify where to locate the curl library]),
            [], [])

AC_ARG_WITH([sasl],
            AS_HELP_STRING([--with-sasl=@<:@=DIR@:>@],
                           [specify where to locate the sasl2 library]),
            [], [])

AC_ARG_WITH([zlib],
            AS_HELP_STRING([--with-zlib=@<:@=DIR@:>@],
                           [specify where to locate the zlib library]),
            [], [])

AC_ARG_WITH([apr],
            AS_HELP_STRING([--with-apr=@<:@=DIR@:>@],
                           [specify where to locate the apr-1 library]),
            [], [])

AC_ARG_WITH([svn],
            AS_HELP_STRING([--with-svn=@<:@=DIR@:>@],
                           [specify where to locate the svn-1 library]),
            [], [])

AC_ARG_WITH([nl],
            AS_HELP_STRING([--with-nl=@<:@DIR@:>@],
                           [specify where to locate the libnl3 library
                            (required for network-isolator). [default: /usr]]),
            [], [])

AC_ARG_ENABLE([bundled-distribute],
              AS_HELP_STRING([--disable-bundled-distribute],
                             [excludes building and using the bundled distribute
                              package in lieu of an installed version in
                              PYTHONPATH]),
              [without_bundled_distribute=yes], [])

AC_ARG_ENABLE([bundled-pip],
              AS_HELP_STRING([--disable-bundled-pip],
                             [excludes building and using the bundled pip
                              package in lieu of an installed version in
                              PYTHONPATH]),
              [without_bundled_pip=yes], [])

AC_ARG_ENABLE([bundled-wheel],
              AS_HELP_STRING([--disable-bundled-wheel],
                             [excludes building and using the bundled wheel
                              package in lieu of an installed version in
                              PYTHONPATH]),
              [without_bundled_wheel=yes], [])

# This is used to make distcheck and python install work together. For more
# information, see the comment above AM_DISTCHECK_CONFIGURE_FLAGS in
# Makefile.am.
AC_ARG_ENABLE([python-dependency-install],
              AS_HELP_STRING([--disable-python-dependency-install],
                             [when the python packages are installed during
                              make install, no external dependencies are
                              downloaded or installed]),
              [without_python_deps=yes], [])

AC_ARG_WITH([network-isolator],
            AS_HELP_STRING([--with-network-isolator],
                           [builds the network isolator]),
            [], [with_network_isolator=no])

AC_ARG_ENABLE([libevent],
              AS_HELP_STRING([--enable-libevent],
                             [use libevent instead of libev default: no]),
              [enable_libevent=yes], [])

AC_ARG_ENABLE([ssl],
              AS_HELP_STRING([--enable-ssl],
                             [use ssl for libprocess communication
                             default: no]),
              [enable_ssl=yes], [])

AC_ARG_WITH([libevent],
            AS_HELP_STRING([--with-libevent=@<:@=DIR@:>@],
                           [specify where to locate the libevent library]),
            [], [])

AC_ARG_WITH([ssl],
            AS_HELP_STRING([--with-ssl=@<:@=DIR@:>@],
                           [specify where to locate the ssl library]),
            [], [])

# TODO(hartem): consolidate all of the ENABLE and _WITH statements
# above in separate sections for better readability.

AC_ARG_VAR([JAVA_HOME], [location of Java Development Kit (JDK)])

AC_ARG_VAR([JAVA_CPPFLAGS], [preprocessor flags for JNI])

AC_ARG_VAR([JAVA_JVM_LIBRARY], [full path to libjvm.so])

AC_ARG_VAR([MAVEN_HOME], [looks for mvn at MAVEN_HOME/bin/mvn])

AC_ARG_VAR([PROTOBUF_JAR], [full path to protobuf jar on prefixed builds])

AC_ARG_VAR([PYTHON], [which Python interpreter to use])

AC_MSG_NOTICE([Setting up build environment for ${target_cpu} ${target_os}])
# Determine the current OS (TODO(benh): Does autotools do this for us?).
case "${target_os}" in
  linux*)
    OS_NAME=linux
    LIBS="$LIBS -lrt"
    ;;
  darwin*)
    OS_NAME=darwin
    ;;
  *)
    AC_MSG_ERROR("Mesos is currently unsupported on your platform.")
esac


# Used for conditionally including source files.
AM_CONDITIONAL([OS_LINUX], [test "x$OS_NAME" = "xlinux"])


# Default to gcc toolchain (we rely on some atomic builtins for now,
# that are also present with clang).
AC_PROG_CXX([g++])
AC_PROG_CC([gcc])

AX_COMPILER_VERSION
AX_COMPILER_VENDOR

# Check if we should enable debugging, optimization. Note we only
# update CFLAGS and CXXFLAGS if none are provided.
AM_CONDITIONAL([DEBUG], [test x"$enable_debug" = "xyes"])
AM_CONDITIONAL([OPTIMIZE], [test x"$enable_optimize" = "xyes"])


debug_flags="-g1"
if test "x$enable_debug" = "xyes"; then
  debug_flags="-g"
elif test "x$enable_optimize" = "xyes"; then
  debug_flags=""
fi

if test "x$enable_optimize" = "xyes"; then
  optimize_flags="-O2"
else
  optimize_flags="-O0"
fi

AS_IF([test "x${ac_cv_env_CFLAGS_set}" = "x"],
      [CFLAGS="$debug_flags $optimize_flags"])
AS_IF([test "x${ac_cv_env_CXXFLAGS_set}" = "x"],
      [CXXFLAGS="$debug_flags $optimize_flags"])


# Attempt to use preinstalled dependencies instead of the bundled
# versions in cases where the user specified their location
# (--with-XXX=DIR) or asked us to attempt to detect them
# (--disable-bundled).


# Check if LevelDB prefix path was supplied and if so, add it to
# CPPFLAGS while extending it by /include and to LDFLAGS while
# extending it by /lib.
if test -n "`echo $with_leveldb`"; then
  CPPFLAGS="$CPPFLAGS -I${with_leveldb}/include"
  LDFLAGS="$LDFLAGS -L${with_leveldb}/lib"
fi


# Check if protobuf prefix path was supplied and if so, add it to
# CPPFLAGS while extending it by /include and to LDFLAGS while
# extending it by /lib.
if test -n "`echo $with_protobuf`"; then
  CPPFLAGS="$CPPFLAGS -I${with_protobuf}/include"
  LDFLAGS="$LDFLAGS -L${with_protobuf}/lib"
fi


# Check if ZooKeeper prefix path was supplied and if so, add it to
# CPPFLAGS while extending it by /include/zookeeper and to LDFLAGS
# while extending it by /lib.
# NOTE: The reason we append /include/zookeeper is because in mesos,
# we include <zookeeper.h> rather than <zookeeper/zookeeper.h>.
if test -n "`echo $with_zookeeper`"; then
  CPPFLAGS="$CPPFLAGS -I${with_zookeeper}/include/zookeeper"
  LDFLAGS="$LDFLAGS -L${with_zookeeper}/lib"
elif test "x$enable_bundled" = "xno"; then
  CPPFLAGS="$CPPFLAGS -I/usr/include/zookeeper"
fi


# Check if glog prefix path was supplied and if so, add it to CPPFLAGS
# while extending it by /include and to LDFLAGS while extending it by
# /lib.
if test -n "`echo $with_glog`"; then
  CPPFLAGS="$CPPFLAGS -I${with_glog}/include"
  LDFLAGS="$LDFLAGS -L${with_glog}/lib"
fi


# Check if user has asked us to use a preinstalled LevelDB, or if they
# asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_leveldb" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([leveldb/db.h],
                   [AC_CHECK_LIB([leveldb],
                                 [leveldb_open],
                                 [found_leveldb=yes])])

  if test "x$found_leveldb" = "xyes"; then
    with_bundled_leveldb=no
  else
    AC_MSG_ERROR([cannot find LevelDB
-------------------------------------------------------------------
You have requested the use of a non-bundled LevelDB but no suitable
LevelDB could be found.

You may want specify the location of LevelDB by providing a prefix
path via --with-leveldb=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_leveldb=yes
fi


AM_CONDITIONAL([WITH_BUNDLED_LEVELDB],
               [test "x$with_bundled_leveldb" = "xyes"])


# Check if user has asked us to use a preinstalled glog, or if they
# asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_glog" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([glog/logging.h],
                   [AC_LINK_IFELSE(
                        [AC_LANG_PROGRAM([#include <glog/logging.h>], [])],
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


AM_CONDITIONAL([WITH_BUNDLED_GLOG],
               [test "x$with_bundled_glog" = "xyes"])


# Check if user has asked us to use a preinstalled ZooKeeper or if
# they asked us to ignore all bundled libraries while compiling and
# linking.
if test "x$without_bundled_zookeeper" = "xyes" || \
   test "x$enable_bundled" != "xyes"; then
  # Check if headers and library were located.
  AC_CHECK_HEADERS([zookeeper.h],
                   [AC_CHECK_LIB([zookeeper_mt],
                                 [zookeeper_init],
                                 [found_zookeeper=yes])])

  if test "x$found_zookeeper" = "xyes"; then
    with_bundled_zookeeper=no
  else
    AC_MSG_ERROR([cannot find ZooKeeper
-------------------------------------------------------------------
You have requested the use of a non-bundled ZooKeeper but no suitable
ZooKeeper could be found.

You may want specify the location of ZooKeeper by providing a prefix
path via --with-zookeeper=DIR, or check that the path you provided is
correct if you're already doing this
-------------------------------------------------------------------
])
  fi
else
  with_bundled_zookeeper=yes
fi


AM_CONDITIONAL([WITH_BUNDLED_ZOOKEEPER],
               [test "x$with_bundled_zookeeper" = "xyes"])


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

  # Check for protobuf.
  # TODO(jieyu): Symbol _init is not available for libs on OS X.
  # Consider checking lib using AX_CXX_CHECK_LIB.
  AC_CHECK_HEADERS([google/protobuf/message.h],
                   [AC_CHECK_LIB([protobuf],
                                 [_init],
                                 [found_protobuf=yes])])

  if test "x$found_protobuf" = "xyes"; then
    with_bundled_protobuf=no
    if test -z "`echo $PROTOBUF_JAR`"; then
      PROTOBUF_JAR="${PROTOBUFPREFIX}/share/java/protobuf.jar"
    fi

    AC_CHECK_FILE([$PROTOBUF_JAR],
                  [],
                  [AC_MSG_ERROR([cannot find PROTOBUF_JAR=$PROTOBUF_JAR])])

    AC_CHECK_TOOL([PROTOCOMPILER_TEST], [protoc], [], [$PROTOBUFPREFIX/bin])

    if test -z "`echo $PROTOCOMPILER_TEST`"; then
      AC_MSG_ERROR([protoc not found in PATH])
    fi

    PROTOCOMPILER="$PROTOBUFPREFIX/bin/protoc"

    if test "x$enable_python" = "xyes"; then
      AC_PYTHON_MODULE([google.protobuf], [yes])
    fi

  else
    AC_MSG_ERROR([cannot find protobuf
-------------------------------------------------------------------
You have requested the use of a non-bundled protobuf but no suitable
protobuf could be found.

You may want specify the location of protobuf by providing a prefix
path via --with-protobuf=DIR, or check that the path you provided is
correct if youre already doing this.
-------------------------------------------------------------------
])
  fi
else
  with_bundled_protobuf=yes
  PROTOBUF_JAR="\${MESOS_BUILD_DIR}/src/java/target/protobuf-java-2.5.0.jar"
fi


AM_CONDITIONAL([WITH_BUNDLED_PROTOBUF],
               [test "x$with_bundled_protobuf" = "xyes"])

AC_SUBST([PROTOBUF_JAR])
AC_SUBST([PROTOCOMPILER])

AS_CASE($ax_cv_cxx_compiler_vendor,
  [clang], [
    # Check if -Wno-unused-local-typedef is needed by checking a sample
    # compilation which contains a local unused typedef.
    # This is needed because Boost 1.53.0 fails to compile with upstream
    # Clang 3.6 without -Wno-unused-local-typedef. Apple LLVM based on
    # Clang 3.6 doesn't have the same behavior.
    AC_LANG_PUSH([C++])
    AC_COMPILE_IFELSE(
    [AC_LANG_SOURCE([[
      #pragma clang diagnostic error "-Wunused-local-typedef"

      int foo()
      {
        typedef int return_type;
        return 5;
      }
    ]])],
    [warn_local_typedefs=no], [warn_local_typedefs=yes])
    AC_LANG_POP([C++])

    if test "x$warn_local_typedefs" = "xyes"; then
      AC_MSG_NOTICE([Disabling warnings about unused local typedefs])
      CXXFLAGS="${CXXFLAGS} -Wno-unused-local-typedef"
    fi
  ], [gnu], [
    # Check for GCC version >= 4.8.
    AX_COMPARE_VERSION([$ax_cv_cxx_compiler_version], [ge], [4.8],
                       [is_ge_gxx48=yes], [is_ge_gxx48=no])
    if test "x$is_ge_gxx48" != "xyes"; then
      # GCC < 4.8 is not supported.
      AC_MSG_ERROR([GCC 4.8 or higher required (found $ax_cv_cxx_compiler_version)])
    fi

    AC_MSG_NOTICE([Setting up CXXFLAGS for g++ version >= 4.8])
    # Boost 1.53.0 fails to compile with GCC 4.8 without
    # -Wno-unused-local-typedefs, and automake does not recognize the
    # flag.
    # TODO(brenden): Remove this when Boost has a resolution.
    CFLAGS="${CFLAGS} -Wno-unused-local-typedefs"
    CXXFLAGS="${CXXFLAGS} -Wno-unused-local-typedefs"

    # Disable 'maybe-uninitialized' warning when "-O2" is enabled due
    # to MESOS-2690. This is short term fix until the GCC bug is fixed.
    case $CXXFLAGS in
      *-O2*) CXXFLAGS="${CXXFLAGS} -Wno-maybe-uninitialized" ;;
      *) ;;
    esac
  ], [
    AC_MSG_WARN([$ax_cv_cxx_compiler_vendor is an unsupported compiler])
  ]
)

# Ensure we can build the C++11 features we expect, and set the --std=
# flag and CXXFLAG environment variable as appropriate.
AX_CXX_COMPILE_STDCXX_11([noext], [mandatory])

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

    # GTEST on OSX needs its own tr1 tuple.
    # TODO(dhamon): Update to gmock 1.7 and pass GTEST_LANG_CXX11 when
    # in C++11 mode.
    CXXFLAGS="$CXXFLAGS -DGTEST_USE_OWN_TR1_TUPLE=1"
    ;;
esac

# Also pass the flags to 3rdparty libraries.
CONFIGURE_ARGS="$CONFIGURE_ARGS CXXFLAGS='$CXXFLAGS'"


# Check for pthreads (uses m4/ax_pthread.m4).
AX_PTHREAD([], [AC_MSG_ERROR([failed to find pthreads])])


# Check for libunwind, and link it in if present.
AC_CHECK_LIB(unwind, backtrace, LIBS="$LIBS -lunwind")


# Check if zlib prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes.
if test -n "`echo $with_zlib`" ; then
    CPPFLAGS="-I${with_zlib}/include $CPPFLAGS"
    LDFLAGS="-L${with_zlib}/lib $LDFLAGS"
fi

AC_CHECK_TOOL([PATCHCOMMAND_TEST], [patch], [], [$PATH])
if test -z "`echo $PATCHCOMMAND_TEST`"; then
    AC_MSG_ERROR([patch command not found in PATH])
fi


AC_CHECK_LIB([z], [gzread], [],
             [AC_MSG_ERROR([cannot find libz
-------------------------------------------------------------------
libz is required for mesos to build.
-------------------------------------------------------------------
])])


# Check if Libcurl prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes.
if test -n "`echo $with_curl`" ; then
    CPPFLAGS="-I${with_curl}/include $CPPFLAGS"
    LDFLAGS="-L${with_curl}/lib $LDFLAGS"
fi

AC_CHECK_LIB([curl], [curl_global_init], [],
             [AC_MSG_ERROR([cannot find libcurl
-------------------------------------------------------------------
libcurl is required for mesos to build.
-------------------------------------------------------------------
])])


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
libapr-1 is required for mesos to build.
-------------------------------------------------------------------
                        ])])], [AC_MSG_ERROR([cannot find libapr-1 headers
-------------------------------------------------------------------
libapr-1 is required for mesos to build.
-------------------------------------------------------------------
])])


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

AC_CHECK_HEADERS([svn_version.h],
                 [AC_CHECK_LIB([svn_subr-1], [svn_stringbuf_create_ensure], [],
                               [AC_MSG_ERROR([cannot find libsvn_subr-1
-------------------------------------------------------------------
libsubversion-1 is required for mesos to build.
-------------------------------------------------------------------
                        ])])], [AC_MSG_ERROR([cannot find libsvn_subr-1 headers
-------------------------------------------------------------------
libsubversion-1 is required for mesos to build.
-------------------------------------------------------------------
])])

AC_CHECK_HEADERS([svn_delta.h],
                 [AC_CHECK_LIB([svn_delta-1], [svn_txdelta], [],
                               [AC_MSG_ERROR([cannot find libsvn_delta-1
-------------------------------------------------------------------
libsubversion-1 is required for mesos to build.
-------------------------------------------------------------------
                        ])])], [AC_MSG_ERROR([cannot find libsvn_delta-1 headers
-------------------------------------------------------------------
libsubversion-1 is required for mesos to build.
-------------------------------------------------------------------
])])


# Check if Sasl2 prefix path was provided, and if so, add it to
# the CPPFLAGS and LDFLAGS with respective /include and /lib path
# suffixes.
if test -n "`echo $with_sasl`" ; then
  CPPFLAGS="-I${with_sasl}/include $CPPFLAGS"
  LDFLAGS="-L${with_sasl}/lib $LDFLAGS"
fi

AC_CHECK_LIB([sasl2], [sasl_done], [],
             [AC_MSG_ERROR([cannot find libsasl2
-------------------------------------------------------------------
We need libsasl2 for authentication!
-------------------------------------------------------------------
])])

AC_MSG_CHECKING([SASL CRAM-MD5 support])

# Render a test binary for checking if the SASL client supports CRAM-MD5.
cat <<__EOF__ >crammd5_installed.c [
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sasl/sasl.h>
#include <sasl/saslplug.h>
#include <sasl/saslutil.h>

static void search_crammd5(
  client_sasl_mechanism_t *m,
  sasl_info_callback_stage_t stage,
  void *rock)
{
  if (stage == SASL_INFO_LIST_START || stage == SASL_INFO_LIST_END) {
    return;
  }
  if (m->plug != NULL) {
    if (!strcmp("CRAM-MD5", m->plug->mech_name)) {
      printf("found");
    }
  }
}

int main(int argc, char** argv)
{
  int result = sasl_client_init(NULL);
  if (result != SASL_OK) {
    return 1;
  }
  sasl_client_plugin_info(NULL, &search_crammd5, NULL);
  return 0;
}]
__EOF__

# Build the SASL client check test binary. Prevent the use of possibly
# activated debug symbols in executables (cc -g) as those cause a
# distcheck failure on OSX by leaking build artefacts (.dsym).
AS_IF([test "x${ac_cv_env_CFLAGS_set}" = "x"],
      [SASL_TEST_CFLAGS=""], [SASL_TEST_CFLAGS=$CFLAGS])
$CC crammd5_installed.c $CPPFLAGS $SASL_TEST_CFLAGS $LDFLAGS -lsasl2  \
  -o crammd5_installed 2>&1 >/dev/null

# Run the test binary and get its output.
crammd5test=`./crammd5_installed 2>&1`

# Clean up all leftovers from this test.
rm -f crammd5_installed.c crammd5_installed

AS_IF([test "x$crammd5test" = "xfound"],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_ERROR([no
-------------------------------------------------------------------
We need CRAM-MD5 support for SASL authentication.
-------------------------------------------------------------------])])


# Perform necessary configuration for network isolator.
if test "x$with_network_isolator" = "xyes"; then
  if test -n "`echo $with_nl`"; then
    CPPFLAGS="-I${with_nl}/include/libnl3 $CPPFLAGS"
    LDFLAGS="-L${with_nl}/lib $LDFLAGS"
  else
    CPPFLAGS="-I/usr/include/libnl3 $CPPFLAGS"
  fi

  # Check for OS support.
  AS_IF([test "$OS_NAME" = "linux"],
        [],
        [AC_MSG_ERROR([cannot build network isolator
-------------------------------------------------------------------
Network isolator is only supported on Linux!
-------------------------------------------------------------------
  ])])

  # Check for libnl (both headers and libraries).
  AC_CHECK_LIB([nl-3], [nl_has_capability], [],
               [AC_MSG_ERROR([cannot find libnl-3
-------------------------------------------------------------------
We need libnl-3 for building network isolator!

Please install libnl3 (version 3.2.26 or higher):
http://www.infradead.org/~tgr/libnl/
-------------------------------------------------------------------
  ])])

  AC_CHECK_HEADERS([netlink/netlink.h libnl3/netlink/netlink.h],
                   [break]
                   [AC_MSG_ERROR([cannot find libnl-3 headers
-------------------------------------------------------------------
We need libnl-3 headers for building network isolator!

Please install libnl3 (version 3.2.26 or higher):
http://www.infradead.org/~tgr/libnl/
-------------------------------------------------------------------
  ])])

  # Check for libnl-route (both headers and libraries).
  AC_CHECK_LIB([nl-route-3], [rtnl_u32_get_classid], [],
               [AC_MSG_ERROR([cannot find libnl-route-3
-------------------------------------------------------------------
We need libnl-route-3 for building network isolator!

Please install libnl3 (version 3.2.26 or higher):
http://www.infradead.org/~tgr/libnl/
-------------------------------------------------------------------
  ])])

  AC_CHECK_HEADERS([netlink/route/link/veth.h libnl3/netlink/route/link/veth.h],
                   [break]
                   [AC_MSG_ERROR([cannot find libnl-route-3 headers
-------------------------------------------------------------------
We need libnl-route-3 headers for building network isolator!

Please install libnl3 (version 3.2.26 or higher):
http://www.infradead.org/~tgr/libnl/
-------------------------------------------------------------------
  ])])

  # Check for libnl-idiag-3 (both headers and libraries).
  AC_CHECK_LIB([nl-idiag-3], [idiagnl_msg_alloc_cache], [],
               [AC_MSG_ERROR([cannot find libnl-idiag-3
-------------------------------------------------------------------
We need libnl-idiag-3 for building network isolator!

Please install libnl3 (version 3.2.26 or higher):
http://www.infradead.org/~tgr/libnl/
-------------------------------------------------------------------
  ])])

  AC_DEFINE([WITH_NETWORK_ISOLATOR])
fi

AM_CONDITIONAL([WITH_NETWORK_ISOLATOR],
               [test "x$with_network_isolator" = "xyes"])


# TODO(benh): Consider using AS_IF instead of just shell 'if'
# statements for better autoconf style (the AS_IF macros also make
# sure variable dependencies are handled appropriately).

# A helper for checking whether we can compile and link using JNI with
# the current JAVA_CPPFLAGS (and JAVA_TEST_LDFLAGS which we setup
# ourselves below).
# TRY_LINK_JNI([ACTION-SUCCESS], [ACTION-FAILURE])
AC_DEFUN([TRY_LINK_JNI], [
  cat <<__EOF__ >conftest.cpp [
#include <jni.h>
int main(int argc, char** argv)
{
  JNIEnv* env;
  JavaVM* jvm;
  JavaVMInitArgs vmArgs;
  return JNI_CreateJavaVM(&jvm, (void**) &env, &vmArgs);
}]
__EOF__

  # Try to compile and link via libtool (the one we generate).
  ./libtool --tag=CXX --mode=link $CXX -Wall -Werror $JAVA_CPPFLAGS \
    -o conftest conftest.cpp $JAVA_TEST_LDFLAGS >&5

  if test $? != 0; then
    rm -f conftest # Cleanup after ourselves.
    $2 # Expand failure action.
  else
    rm -f conftest # Cleanup after ourselves.
    $1 # Expand success action.
  fi
])


# Perform necessary configuration for building with Java.
if test "x$enable_java" = "xyes"; then
  # First let's try and determine JAVA_HOME if it hasn't been set. We
  # do this by checking to see if the directory found at the
  # 'java.home' system property for the java found on the path
  # includes javac. If so, then we'll guess that this is a JDK
  # installation rather than a JRE installation.
  if test -z "$JAVA_HOME"; then
    AC_PATH_PROG([JAVAC], [javac], [$JAVAC])
    AC_PATH_PROG([JAVA], [java], [$JAVA])

    if test "x$JAVA" = "x" || test "x$JAVAC" = "x"; then
      AC_MSG_ERROR([can not guess JAVA_HOME (no 'java' or 'javac' found)])
    fi

    AC_MSG_CHECKING([value of Java system property 'java.home'])
    cat <<__EOF__ >conftest.java [
public class conftest {
  public static void main(String[] args) {
    System.out.print(System.getProperty("java.home"));
  }
}]
__EOF__

    # Now build and run the code.
    $JAVAC conftest.java && JAVA_DOT_HOME="`$JAVA -cp . conftest`"

    if test $? = 0 && test ! -z "$JAVA_DOT_HOME"; then
      AC_MSG_RESULT($JAVA_DOT_HOME)
    else
      JAVA_DOT_HOME=""
      AC_MSG_RESULT([not found])
    fi

    # Clean up after ourselves.
    rm -f conftest.java conftest.class

    # Check if 'java.home' looks like a JDK installation, or if
    # 'java.home'/.. looks like a JDK installation (e.g., Linux).
    if test -f $JAVA_DOT_HOME/bin/javac; then
      JAVA_HOME=$JAVA_DOT_HOME
    elif test -f `dirname $JAVA_DOT_HOME`/bin/javac; then
      JAVA_HOME=`dirname $JAVA_DOT_HOME`
    fi

    if test -z "$JAVA_HOME"; then
      AC_MSG_ERROR([could not guess JAVA_HOME])
    else
      AC_MSG_NOTICE([using JAVA_HOME=$JAVA_HOME])
    fi
  fi

  # Determine linker flags for Java if not set.
  if test "$OS_NAME" = "darwin"; then
    dir="$JAVA_HOME/jre/lib/server"
    JAVA_TEST_LDFLAGS="-framework JavaVM"
    JAVA_JVM_LIBRARY=$dir/libjvm.dylib
  elif test "$OS_NAME" = "linux"; then
    for arch in amd64 i386 arm aarch64 ppc64 ppc64le s390 s390x; do
      dir="$JAVA_HOME/jre/lib/$arch/server"
      if test -e "$dir"; then
	# Note that these are libtool specific flags.
	JAVA_TEST_LDFLAGS="-L$dir -R$dir -Wl,-ljvm"
	JAVA_JVM_LIBRARY=$dir/libjvm.so
	break;
      fi
    done
  fi

  if test -z "$JAVA_TEST_LDFLAGS"; then
    AC_MSG_ERROR([failed to determine linker flags for using Java \
(bad JAVA_HOME or missing support for your architecture?)])
  fi

  # Now try and build with JNI, looping through possible compiler
  # flags as necessary (provided JAVA_CPPFLAGS was not set).
  AC_MSG_CHECKING([whether or not we can build with JNI])
  if test -z "$JAVA_CPPFLAGS"; then
    if test "$OS_NAME" = "darwin"; then
      while true; do # Loop until sucessful (via break) or exhausted options.
        m4_foreach([java_cppflags],
                   [["-I$JAVA_HOME/include -I$JAVA_HOME/include/$OS_NAME"],
                    ["-I/System/Library/Frameworks/JavaVM.framework/Headers"]],
                   [JAVA_CPPFLAGS=java_cppflags
                    TRY_LINK_JNI([break])])
        # Exhausted options.
        AC_MSG_ERROR([failed to build with JNI
  -------------------------------------------------------------------
  It appears we were unable to compile against the JNI. This is most
  likely due to one of the following issues:
    1. You do not have a JDK installed on your system.
    2. All JDKs installed on your system have deprecated JNI headers.
  It is advised to install OpenJDK on your system, as the JDK that
  ships with OS X has deprecated JNI headers.
  -------------------------------------------------------------------
        ])
      done
    else
      while true; do # Loop until sucessful (via break) or exhausted options.
        m4_foreach([java_cppflags],
                   [["-I$JAVA_HOME/include -I$JAVA_HOME/include/$OS_NAME"]],
                   [JAVA_CPPFLAGS=java_cppflags
                    TRY_LINK_JNI([break])])
        # Exhausted options.
        AC_MSG_ERROR([failed to build with JNI])
      done
    fi
  else
    TRY_LINK_JNI([], [AC_MSG_ERROR([failed to build with JNI])])
  fi
  AC_MSG_RESULT([yes])

  AC_CONFIG_FILES([src/examples/java/test-exception-framework],
                  [chmod +x src/examples/java/test-exception-framework])
  AC_CONFIG_FILES([src/examples/java/test-executor],
                  [chmod +x src/examples/java/test-executor])
  AC_CONFIG_FILES([src/examples/java/test-framework],
                  [chmod +x src/examples/java/test-framework])
  AC_CONFIG_FILES([src/examples/java/test-multiple-executors-framework],
                  [chmod +x src/examples/java/test-multiple-executors-framework])
  AC_CONFIG_FILES([src/examples/java/test-log],
                  [chmod +x src/examples/java/test-log])
  AC_CONFIG_FILES([src/java/mesos.pom])

  AC_DEFINE([MESOS_HAS_JAVA])

  has_java=yes

  # Test for Maven so we can build the jar.
  if test -z "$MAVEN_HOME"; then
    AC_PATH_PROG([MVN], [mvn], [$MVN])

    if test "x$MVN" = "x"; then
      AC_MSG_ERROR([can not find mvn on your path
-------------------------------------------------------------------
You may want verify that Maven has been correctly installed and
added to your PATH or set the MAVEN_HOME environment variable
to point to your Maven installation.
-------------------------------------------------------------------
])
    fi
  else
    AC_PATH_PROGS([MVN], [mvn], [$MVN], [$MAVEN_HOME/bin])

    if test "x$MVN" = "x"; then
      AC_MSG_ERROR([can not find mvn inside $MAVEN_HOME/bin
-------------------------------------------------------------------
You may want verify the MAVEN_HOME validity or check that Maven
has been correctly installed.
-------------------------------------------------------------------
])
    fi
  fi
fi

AM_CONDITIONAL([HAS_JAVA], [test "x$has_java" = "xyes"])


# Perform necessary configuration for building with Python.
if test "x$enable_python" = "xyes"; then
  # If the user specified PYTHON_VERSION then assume that is the
  # Python that they want and construct PYTHON using the canonical
  # name (failing if PYTHON is also set since we can't easily check
  # that PYTHON and PYTHON_VERSION are compatible). This bit of
  # tomfoolery is mostly to match the semantics of AC_PYTHON_DEVEL
  # (used below). If we don't do this then it's possible that a user
  # will specify PYTHON_VERSION which will get ignored (and likely
  # replaced) via AM_PATH_PYTHON.
  # TODO(benh): Consolidate AM_PATH_PYTHON and AC_PYTHON_DEVEL into a
  # single macro that both iterates through potential 'python' options
  # looking for an acceptable version (i.e., AM_PATH_PYTHON) and
  # checks for Python.h (i.e., AC_PYTHON_DEVEL).
  if test -n "$PYTHON_VERSION"; then
    if test -n "$PYTHON"; then
      AC_MSG_ERROR([only specify one of PYTHON or PYTHON_VERSION])
    fi
    PYTHON=python$PYTHON_VERSION
  fi

  # Check if PYTHON is at least 2.6 or try and find one that is if
  # PYTHON is not set (necessary to run our examples).
  AM_PATH_PYTHON([2.6],,
                 [AC_MSG_ERROR([mesos requires Python >= 2.6
-------------------------------------------------------------------
If you already have Python 2.6 installed (and it's on the path),
you might want to check if you have the PYTHON environment variable
set to an older version of Python.
-------------------------------------------------------------------
  ])])

  # Next we ensure we have the Python development libraries.
  AX_PYTHON_DEVEL([>= '2.6'])

  # Ensure that we can build a native Python egg linking against a
  # static library that imports from the standard C++ library.
  # This was added due to MESOS-799.
  AC_MSG_CHECKING([whether we can build usable Python eggs])

  # Render code that imports from the standard C++ library.
  cat <<__EOF__ >testlib.cpp [
#include <string>
std::string test(void)
{
  return std::string("42");
}]
__EOF__

  # Build a static library from the above code.
  $CXX -x c++ -fPIC -c testlib.cpp $CPPFLAGS $CFLAGS $CXXFLAGS \
    $LDFLAGS -o testlib.o
  ar rcs libtest.a testlib.o

  # Render a native Python module that imports from that library.
  cat <<__EOF__ >testpyegg.cpp [
#include <Python.h>
#include <string>

std::string test(void);
PyMODINIT_FUNC initminimal(void);

static PyObject *minimal_test(PyObject *self, PyObject* args)
{
  return PyString_FromString(test().c_str());
}

static PyMethodDef minimal_methods[] = {
  { "test", minimal_test, METH_NOARGS, "Test." },
  { NULL, NULL }
};

PyMODINIT_FUNC initminimal(void)
{
  PyImport_AddModule("minimal");
  Py_InitModule("minimal", minimal_methods);
}]
__EOF__

  # Render a distutils setup for building the Python egg.
  cat <<__EOF__ >setup.py [
from distutils.core import setup, Extension;
setup(name = 'MinimalTest',
      version = '1.0',
      description = 'foo',
      ext_modules = [ Extension('minimal',
                                 extra_objects = ['libtest.a'],
                                 sources = ['testpyegg.cpp'] ) ] )
]
__EOF__

  # Build that Python egg that links against that library. The build
  # settings are propagated to distutils.
  # NOTE: We need to embed our CXXFLAGS in CFLAGS because distutils
  # doesn't pull out CXXFLAGS automagically.
  CXX="$CXX" CC="$CC" CFLAGS="$CFLAGS $CXXFLAGS" \
    LDFLAGS="$LDFLAGS" $PYTHON setup.py build_ext --inplace \
    --build-temp ./ 2>&1 >/dev/null

  # Run a test-script that makes use of the Python egg.
  pyeggtest=`$PYTHON -c "import minimal; print(minimal.test())" 2>&1`

  # Validate the test-script output.
  AS_IF([test "x$pyeggtest" = "x42"], [pyeggbuild=yes])

  # Clean up all leftovers from this test.
  rm -f testlib.cpp testlib.o libtest.a
  rm -f testpyegg.cpp testpyegg.o setup.py minimal.so

  AS_IF([test "x$pyeggbuild" = "xyes"],
        [AC_MSG_RESULT([yes])],
        [AS_IF([test "x$OS_NAME" = "xdarwin"],
               [AC_MSG_ERROR([no
-------------------------------------------------------------------
It appears we were unable to build a usable native Python egg. You
might be using the default Mac OS X Python distribution which is
incompatible for building native eggs using this compiler setup.
For more details, see:
    https://issues.apache.org/jira/browse/MESOS-799

There are two possible workarounds for this issue:
    1. Disable python bindings by configuring with --disable-python.
    2. Use an alternative Python installation that was built using
       the same build setup as Mesos.
-------------------------------------------------------------------])],
               [AC_MSG_ERROR([no
-------------------------------------------------------------------
It appears we were unable to build a usable native Python egg.

There are two possible workarounds for this issue:
    1. Disable python bindings by configuring with --disable-python.
    2. Use an alternative Python installation that was built using
       the same build setup as Mesos.
-------------------------------------------------------------------])])
        ])

  AC_MSG_CHECKING([for an old installation of the Mesos egg (before 0.20.0)])

  $PYTHON -c "import mesos; mesos._mesos" &> /dev/null

  if test $? = 0; then
    pymodulelocation=`python -c \
      "import mesos; import os; print os.path.dirname(mesos.__path__[[0]])" \
      2> /dev/null`
  fi

  AS_IF([test -z "$pymodulelocation"],
        [AC_MSG_RESULT([no])],
        [AC_MSG_ERROR([yes
-------------------------------------------------------------------
It appears that you currently have a native Python egg installed
from a version before 0.20.0. This conflicts with the egg in this
version.

There are two possible workarounds for this issue:
    1. Disable Python bindings by configuring with --disable-python.
    2. Uninstall the legacy egg from your Python installation. This
       might require you doing:
         rm -rf $pymodulelocation
-------------------------------------------------------------------])])

  # Determine how the generated Python egg's will get named, used in
  # the Makefile to keep the targets from being rerun.
  PYTHON_EGG_POSTFIX=`$PYTHON -c \
    'import sys; \
     from distutils.util import get_platform; \
     print "-py" + sys.version[[0:3]] + "-" + get_platform()'`

  PYTHON_EGG_PUREPY_POSTFIX=`$PYTHON -c \
    'import sys; \
     from distutils.util import get_platform; \
     print "-py" + sys.version[[0:3]]'`

  PYTHON_WHL_POSTFIX=`$PYTHON -c \
    'import sys; \
     from distutils.util import get_platform; \
     print "-cp" + sys.version[[0:3]].replace(".", "") + "-none-" \
       + get_platform().replace(".", "_").replace("-", "_")'`

  PYTHON_WHL_PUREPY_POSTFIX=`$PYTHON -c \
    'import sys; \
     print "-py" + sys.version[[0]] + "-none-any"'`

  AC_CONFIG_FILES([src/examples/python/test-executor],
                  [chmod +x src/examples/python/test-executor])
  AC_CONFIG_FILES([src/examples/python/test-framework],
                  [chmod +x src/examples/python/test-framework])
  AC_CONFIG_FILES([src/examples/python/test-containerizer],
                  [chmod +x src/examples/python/test-containerizer])
  AC_CONFIG_FILES([src/python/setup.py])
  AC_CONFIG_FILES([src/python/interface/setup.py])
  AC_CONFIG_FILES([src/python/native/ext_modules.py])
  AC_CONFIG_FILES([src/python/native/setup.py])

  # When clang is being used, make sure that the distutils python-
  # config cflags extraction does not cause build errors (MESOS-1079).
  # TODO(tillt): Remove this once Apple distributed an updated Python.
  PYTHON_CPPFLAGS="$CPPFLAGS"
  PYTHON_CFLAGS="$CFLAGS $CXXFLAGS" # distutils requires we embed CXXFLAGS.
  PYTHON_LDFLAGS="$LDFLAGS"

  AS_IF([test "x$ax_cv_cxx_compiler_vendor" = "xclang"],
	[PYTHON_CPPFLAGS="$PYTHON_CPPFLAGS -Qunused-arguments"
         PYTHON_CFLAGS="$PYTHON_CFLAGS -Qunused-arguments"])

  AC_SUBST([PYTHON_CPPFLAGS])
  AC_SUBST([PYTHON_CFLAGS])
  AC_SUBST([PYTHON_EGG_POSTFIX])
  AC_SUBST([PYTHON_EGG_PUREPY_POSTFIX])
  AC_SUBST([PYTHON_WHL_POSTFIX])
  AC_SUBST([PYTHON_WHL_PUREPY_POSTFIX])
  AC_SUBST([PYTHON]) # Used by the example shell scripts and src/Makefile.am.

  AC_DEFINE([MESOS_HAS_PYTHON])

  AC_PYTHON_MODULE([boto], [no],
		   AC_MSG_WARN([can not find python-boto
-------------------------------------------------------------------
mesos-ec2 services will not function.
-------------------------------------------------------------------]))

  # Check if user has asked us to use a preinstalled distribute, or if
  # they asked us to ignore all bundled libraries while compiling and linking.
  if test "x$without_bundled_distribute" = "xyes" || \
     test "x$enable_bundled" != "xyes"; then

    AC_PYTHON_MODULE([distutils], [no])
    if test "x$HAVE_PYMOD_distutils" = "xno"; then
      AC_PYTHON_MODULE([setuptools], [no])

      if test "x$HAVE_PYMOD_setuptools" = "xyes"; then
        found_distribute=yes
      fi

    else
      found_distribute=yes
    fi

    if test "x$found_distribute" = "xyes"; then
      with_bundled_distribute=no
    else
      AC_MSG_ERROR([cannot find distribute
-------------------------------------------------------------------
You have requested the use of a non-bundled distribute but no suitable
distribute could be found.

You may want specify the location of distribute by providing a prefix
path via --with-distribute=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
    fi
  else
    with_bundled_distribute=yes
  fi

  # Check if user has asked us to use a preinstalled pip, or if
  # they asked us to ignore all bundled libraries while compiling and linking.
  if test "x$without_bundled_pip" = "xyes" || \
     test "x$enable_bundled" != "xyes"; then

    AC_PYTHON_MODULE([pip], [no])
    if test "x$HAVE_PYMOD_pip" = "xno"; then
      AC_MSG_ERROR([cannot find pip
-------------------------------------------------------------------
You have requested the use of a non-bundled pip but no suitable
pip could be found.

You may want specify the location of pip by providing a prefix
path via --with-pip=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
    else
      with_bundled_pip=no
    fi
  else
    with_bundled_pip=yes
  fi

  # Check if user has asked us to use a preinstalled wheel, or if
  # they asked us to ignore all bundled libraries while compiling and linking.
  if test "x$without_bundled_wheel" = "xyes" || \
     test "x$enable_bundled" != "xyes"; then

    AC_PYTHON_MODULE([wheel], [no])
    if test "x$HAVE_PYMOD_wheel" = "xno"; then
      AC_MSG_ERROR([cannot find wheel
-------------------------------------------------------------------
You have requested the use of a non-bundled wheel but no suitable
wheel could be found.

You may want specify the location of wheel by providing a prefix
path via --with-wheel=DIR, or check that the path you provided is
correct if you're already doing this.
-------------------------------------------------------------------
])
    else
      with_bundled_wheel=no
    fi
  else
    with_bundled_wheel=yes
  fi

  has_python=yes
fi

AM_CONDITIONAL([HAS_PYTHON], [test "x$has_python" = "xyes"])

AM_CONDITIONAL([WITH_BUNDLED_DISTRIBUTE],
    [test "x$with_bundled_distribute" = "xyes"])
AM_CONDITIONAL([WITH_BUNDLED_PIP],
    [test "x$with_bundled_pip" = "xyes"])
AM_CONDITIONAL([WITH_BUNDLED_WHEEL],
    [test "x$with_bundled_wheel" = "xyes"])

AM_CONDITIONAL([WITHOUT_PYTHON_DEPS],
    [test "x$without_python_deps" = "xyes"])

AM_CONDITIONAL([GIT_REPO], [test -d ${srcdir}"/.git"])


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
  AC_CHECK_HEADERS([event2/event.h],
                   [AC_CHECK_LIB([event],
                                 [event_base_new],
                                 [],
                                 [AC_MSG_ERROR([cannot find libevent
-------------------------------------------------------------------
libevent is required for libprocess to build.
-------------------------------------------------------------------
                                 ])])],
                   [AC_MSG_ERROR([cannot find libevent headers
-------------------------------------------------------------------
libevent is required for libprocess to build.
-------------------------------------------------------------------
  ])])

  AC_CHECK_HEADERS([event2/thread.h],
                   [AC_CHECK_LIB([event_pthreads],
                                 [evthread_use_pthreads],
                                 [],
                                 [AC_MSG_ERROR([cannot find libevent_pthreads
-------------------------------------------------------------------
libevent_pthreads is required for libprocess to build.
-------------------------------------------------------------------
                                 ])])],
                   [AC_MSG_ERROR([cannot find libevent_pthreads headers
-------------------------------------------------------------------
libevent_pthreads is required for libprocess to build.
-------------------------------------------------------------------
  ])])
fi

AM_CONDITIONAL([ENABLE_LIBEVENT], [test x"$enable_libevent" = "xyes"])


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
  CPPFLAGS="-I${with_ssl}/include $CPPFLAGS"
  LDFLAGS="-L${with_ssl}/lib $LDFLAGS"
fi

if test "x$enable_ssl" = "xyes"; then
  AC_CHECK_HEADERS([openssl/ssl.h],
                   [AC_CHECK_LIB([ssl],
                                 [SSL_CTX_new],
                                 [],
                                 [AC_MSG_ERROR([cannot find libssl
-------------------------------------------------------------------
libssl is required for an SSL-enabled build.
-------------------------------------------------------------------
                                 ])])],
                   [AC_MSG_ERROR([cannot find libssl headers
-------------------------------------------------------------------
libssl is required for an SSL-enabled build.
-------------------------------------------------------------------
  ])])

  AC_CHECK_LIB([crypto], [RAND_poll], [], [AC_MSG_ERROR([cannot find libcrypto
-------------------------------------------------------------------
libcrypto is required for an SSL-enabled build.
-------------------------------------------------------------------
  ])])

  if test "x$enable_libevent" = "xyes"; then
    AC_CHECK_HEADERS([event2/bufferevent_ssl.h],
                     [AC_CHECK_LIB([event_openssl],
                                   [bufferevent_openssl_get_ssl],
                                   [],
                                   [AC_MSG_ERROR([cannot find libevent_openssl
-------------------------------------------------------------------
libevent_openssl is required for an SSL-enabled build.
-------------------------------------------------------------------
                                   ])])],
                     [AC_MSG_ERROR([cannot find libevent_openssl headers
-------------------------------------------------------------------
libevent_openssl is required for an SSL-enabled build.
-------------------------------------------------------------------
    ])])
  else
    AC_MSG_ERROR([SSL is currently only supported with libevent])
  fi
  AC_DEFINE([USE_SSL_SOCKET], [1])
fi

AM_CONDITIONAL([ENABLE_SSL], [test x"$enable_ssl" = "xyes"])


# Provide more granular version numbers based on the version string,
# using the format MAJOR.MINOR.PATCH[-SUFFIX], where SUFFIX can itself
# contain dashes.
#
# NOTE: This definition must not be named such that it conflicts with
# existing definitions, e.g., 'version', 'ver'.
m4_define([major_minor_patch],
          m4_split(m4_translit(AC_PACKAGE_VERSION, [-], [.]), [\.]))

AC_SUBST(MESOS_MAJOR_VERSION, m4_car(major_minor_patch))
AC_SUBST(MESOS_MINOR_VERSION, m4_car(m4_shift(major_minor_patch)))
AC_SUBST(MESOS_PATCH_VERSION, m4_car(m4_shift2(major_minor_patch)))


AC_OUTPUT
