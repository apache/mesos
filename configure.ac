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
AC_INIT([mesos], [0.12.0])

# Have autoconf setup some variables related to the system.
AC_CANONICAL_HOST
AC_CANONICAL_BUILD
AC_CANONICAL_TARGET

AC_LANG([C++])

AC_CONFIG_MACRO_DIR([m4])

# Initialize automake.
# -Wno-portability, since we require GNU Make for % patterns
AM_INIT_AUTOMAKE([-Wall -Werror -Wno-portability foreign])

# This is required for linking non-POSIX libs.
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

# Force configured third_party libraries (currently only libprocess)
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

AC_CONFIG_SUBDIRS([third_party/libprocess])

AC_CONFIG_FILES([Makefile])
AC_CONFIG_FILES([ec2/Makefile])
AC_CONFIG_FILES([hadoop/Makefile])
AC_CONFIG_FILES([src/Makefile])
AC_CONFIG_FILES([third_party/Makefile])

AC_CONFIG_FILES([bin/mesos-build-env.sh])
AC_CONFIG_FILES([bin/mesos-local.sh], [chmod +x bin/mesos-local.sh])
AC_CONFIG_FILES([bin/mesos-master.sh], [chmod +x bin/mesos-master.sh])
AC_CONFIG_FILES([bin/mesos-slave.sh], [chmod +x bin/mesos-slave.sh])
AC_CONFIG_FILES([bin/mesos-tests.sh], [chmod +x bin/mesos-tests.sh])
AC_CONFIG_FILES([bin/gdb-mesos-local.sh], [chmod +x bin/gdb-mesos-local.sh])
AC_CONFIG_FILES([bin/gdb-mesos-master.sh], [chmod +x bin/gdb-mesos-master.sh])
AC_CONFIG_FILES([bin/gdb-mesos-slave.sh], [chmod +x bin/gdb-mesos-slave.sh])
AC_CONFIG_FILES([bin/gdb-mesos-tests.sh], [chmod +x bin/gdb-mesos-tests.sh])

AC_CONFIG_FILES([src/deploy/mesos-daemon.sh])
AC_CONFIG_FILES([src/deploy/mesos-start-cluster.sh])
AC_CONFIG_FILES([src/deploy/mesos-start-masters.sh])
AC_CONFIG_FILES([src/deploy/mesos-start-slaves.sh])
AC_CONFIG_FILES([src/deploy/mesos-stop-cluster.sh])
AC_CONFIG_FILES([src/deploy/mesos-stop-masters.sh])
AC_CONFIG_FILES([src/deploy/mesos-stop-slaves.sh])

AC_CONFIG_FILES([include/mesos/mesos.hpp])

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

AC_ARG_ENABLE([optimize],
              AS_HELP_STRING([--disable-optimize],
                             [don't try to compile with optimizations]),
              [], [enable_optimize=yes])

AC_ARG_WITH([curl],
            AS_HELP_STRING([--without-curl],
                           [builds Mesos assuming libcurl
                           (and its dependencies) are available]),
             [], [with_curl=yes])

AC_ARG_WITH([included-zookeeper],
            AS_HELP_STRING([--without-included-zookeeper],
                           [excludes building and using the included ZooKeeper
                           package in lieu of a system installed version (note,
                           however, that no attempt is made to find the package
                           and explicitly setting CPPFLAGS and LDFLAGS as
                           appropriate may be necessary)]),
             [], [with_included_zookeeper=yes])

# TODO(benh): Support --without-included-protobuf,
# --without-included-glog, etc. Doing this for protobuf is
# considerably more tricky because we need to make sure that 'protoc'
# exists, that a protobuf JAR exists or we can make one, that a
# protobuf egg exists or we can make one, etc.

AC_ARG_VAR([JAVA_HOME], [location of Java Development Kit (JDK)])

AC_ARG_VAR([JAVA_CPPFLAGS], [preprocessor flags for JDK])

AC_ARG_VAR([JAVA_LDFLAGS], [linker flags for JDK])

AC_ARG_VAR([PYTHON], [which Python interpreter to use])


# Determine the current OS (TODO(benh): Does autotools do this for us?).
case "${target_os}" in
  linux*)
    echo ===========================================================
    echo Setting up build environment for ${target_cpu} ${target_os}
    echo ===========================================================
    OS_NAME=linux
    LIBS="$LIBS -lrt"
    ;;
  darwin*)
    echo ===========================================================
    echo Setting up build environment for ${target_cpu} ${target_os}
    echo ===========================================================
    OS_NAME=darwin
    ;;
  solaris*)
    echo ===========================================================
    echo Setting up build environment for ${target_cpu} ${target_os}
    echo ===========================================================
    OS_NAME=solaris
    CC=gcc-4.3.2
    CXX=g++-4.3.2
    CFLAGS="$CFLAGS -pthreads -march=i586"
    CXXFLAGS="$CXXFLAGS -pthreads -march=i586"
    LIBS="$LIBS -lsocket -lnsl -lproject -lproc"
    ;;
  *)
    AC_MSG_ERROR("Mesos is currently unsupported on your platform.")
  esac


# Used for conditionally including source files.
AM_CONDITIONAL([OS_LINUX], [test "x$OS_NAME" = "xlinux"])


# Checks for gcc toolchain (we rely on some atomic builtins for now).
AC_PROG_CXX([g++])
AC_PROG_CC([gcc])


# Check for pthreads (uses m4/acx_pthread.m4).
ACX_PTHREAD([], [AC_MSG_ERROR([failed to find pthreads])])


# Check for libunwind, and link it in if present.
AC_CHECK_LIB(unwind, backtrace, LIBS="$LIBS -lunwind")


# TODO(benh): Consider using AS_IF instead of just shell 'if'
# statements for better autoconf style (the AS_IF macros also make
# sure variable dependencies are handled appropriately).

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

  # Determine preprocessor flags for Java if not set.
  if test -z "$JAVA_CPPFLAGS"; then
    JAVA_CPPFLAGS="-I$JAVA_HOME/include -I$JAVA_HOME/include/$OS_NAME"

    # For OSX, set JAVA_CPPFLAGS for 10.7 and above since the JAVA_HOME
    # discovered during configure using 'java.home' does not include
    # the JNI headers. This happens when users have not configured
    # their jdk using the 'Java Preferences' application.
    if test "$OS_NAME" = "darwin"; then
      if test ! -z `sw_vers -productVersion | grep -E '10.7|10.8'`; then
        JAVA_CPPFLAGS="-I/System/Library/Frameworks/JavaVM.framework/Headers"
      fi
    fi
  fi

  # Determine linker flags for Java if not set.
  if test -z "$JAVA_LDFLAGS"; then
    if test "$OS_NAME" = "darwin"; then
      JAVA_LDFLAGS="-framework JavaVM"
    elif test "$OS_NAME" = "linux"; then
      JAVA_LDFLAGS=""
      for arch in amd64 i386; do
        dir="$JAVA_HOME/jre/lib/$arch/server"
        if test -e "$dir"; then
          # Note that these are libtool specific flags.
          JAVA_LDFLAGS="-L$dir -R$dir -Wl,-ljvm"
          break;
          fi
      done
    fi
  fi

  if test -z "$JAVA_LDFLAGS"; then
    AC_MSG_ERROR([failed to determine linker flags for using Java \
(bad JAVA_HOME or missing support for your architecture?)])
  fi

  # Now check that we can build against the JDK (using libtool).
  AC_MSG_CHECKING([whether or not we can build using the JDK])
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
  ./libtool --tag=CXX --mode=link \
    $CXX $JAVA_CPPFLAGS -o conftest conftest.cpp $JAVA_LDFLAGS >&5

  if test $? != 0; then
    AC_MSG_ERROR([failed to build against JDK (using libtool)])
  else
    AC_MSG_RESULT([yes])
  fi

  # Cleanup after ourselves.
  rm -f conftest

  AC_CONFIG_FILES([src/examples/java/test-exception-framework],
                  [chmod +x src/examples/java/test-exception-framework])
  AC_CONFIG_FILES([src/examples/java/test-executor],
                  [chmod +x src/examples/java/test-executor])
  AC_CONFIG_FILES([src/examples/java/test-framework],
                  [chmod +x src/examples/java/test-framework])
  AC_CONFIG_FILES([src/examples/java/test-multiple-executors-framework],
                  [chmod +x src/examples/java/test-multiple-executors-framework])
  AC_CONFIG_FILES([src/java/mesos.pom])

  AC_DEFINE([MESOS_HAS_JAVA])

  has_java=yes
fi

AM_CONDITIONAL([HAS_JAVA], [test "x$has_java" = "xyes"])


# Perform necessary configuration for building with Python.
if test "x$enable_python" = "xyes"; then
  AM_PATH_PYTHON([2.6],,
                 [AC_MSG_ERROR([mesos requires Python >= 2.6 to be installed
  -------------------------------------------------------------------
  If you already have Python 2.6 installed, please ensure you have
  not set the PYTHON environment variable to point to an older
  version of Python.
  -------------------------------------------------------------------
  ])])

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

  AC_CONFIG_FILES([src/examples/python/test-executor],
                  [chmod +x src/examples/python/test-executor])
  AC_CONFIG_FILES([src/examples/python/test-framework],
                  [chmod +x src/examples/python/test-framework])
  AC_CONFIG_FILES([src/python/setup.py])

  AC_SUBST([PYTHON_EGG_POSTFIX])
  AC_SUBST([PYTHON_EGG_PUREPY_POSTFIX])
  AC_SUBST([PYTHON]) # Used by the example shell scripts and src/Makefile.am.

  AC_DEFINE([MESOS_HAS_PYTHON])

  has_python=yes
fi

AM_CONDITIONAL([HAS_PYTHON], [test "x$has_python" = "xyes"])


# Check if we should try and enable optimizations.
if test "x$enable_optimize" = "xyes"; then
  # For now, we only turn on optimizations for gcc.
  if test "x$GCC" = "xyes"; then
    CXXFLAGS="$CXXFLAGS -g2 -O2"
  fi
fi


# Check if we should/can build with libcurl.
if test "x$with_curl" = "xyes"; then
  AC_CHECK_LIB([z], [gzread], [],
               [AC_MSG_ERROR([cannot find libz
  -------------------------------------------------------------------
  We need libz for libcurl; you can avoid this with --without-curl,
  but it will mean executor and task resources cannot be downloaded
  over http.
  -------------------------------------------------------------------
  ])])

  AC_CHECK_LIB([crypto], [BN_init], [],
               [AC_MSG_ERROR([cannot find libcrypto
  -------------------------------------------------------------------
  We need libcrypto for libcurl; you can avoid this with
  --without-curl, but it will mean executor and task resources cannot
  be downloaded over http.
  -------------------------------------------------------------------
  ])])

  AC_CHECK_LIB([ssl], [SSL_accept], [],
               [AC_MSG_ERROR([cannot find libssl
  -------------------------------------------------------------------
  We need libssl for libcurl; you can avoid this with --without-curl,
  but it will mean executor and task resources cannot be downloaded
  over http.
  -------------------------------------------------------------------
  ])])

  AC_CHECK_LIB([curl], [curl_global_init], [],
               [AC_MSG_ERROR([cannot find libcurl
  -------------------------------------------------------------------
  You can avoid this with --without-curl, but it will mean executor
  and task resources cannot be downloaded over http.
  -------------------------------------------------------------------
  ])])

fi


AM_CONDITIONAL([WITH_INCLUDED_ZOOKEEPER],
               [test "x$with_included_zookeeper" = "xyes"])


AC_OUTPUT
