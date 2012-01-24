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

AC_PREREQ(2.61)
AC_INIT([mesos], [0.0.1])

# Have autoconf setup some variables related to the system.
AC_CANONICAL_HOST
AC_CANONICAL_BUILD
AC_CANONICAL_TARGET

# Initialize automake.
AM_INIT_AUTOMAKE([-Wall -Werror foreign])

# Initialize libtool (LT_OUTPUT builds ./libtool immediately, needed
# so we can do tests with libtool during configuration).
LT_PREREQ([2.2])
LT_INIT
LT_LANG([C++])
LT_OUTPUT

AC_LANG(C++)

AC_CONFIG_MACRO_DIR([m4])

# The default CFLAGS/CXXFLAGS from autoconf when using gcd is usually
# "-g -O2". These really slow down compiling our tests, so we turn
# them off and enable them (where desired) directly in the
# Makefile. Note that this should not have an impact on users setting
# CFLAGS/CXXFLAGS directly at configure time, or when running make.
AS_IF([test "x${ac_cv_env_CFLAGS_set}" = "x"],
      [CFLAGS=""])
AS_IF([test "x${ac_cv_env_CXXFLAGS_set}" = "x"],
      [CXXFLAGS=""])

# Force all third_party libraries to only build static libraries so
# that we can produce a final shared library which includes everything
# necessary.
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
AC_CONFIG_SUBDIRS([third_party/protobuf-2.3.0])
AC_CONFIG_SUBDIRS([third_party/glog-0.3.1])

AC_CONFIG_FILES([Makefile])
AC_CONFIG_FILES([src/Makefile])
AC_CONFIG_FILES([src/config/config.hpp])
AC_CONFIG_FILES([bin/mesos-build-env.sh])
AC_CONFIG_FILES([bin/mesos-master.sh], [chmod +x bin/mesos-master.sh])
AC_CONFIG_FILES([bin/mesos-slave.sh], [chmod +x bin/mesos-slave.sh])
AC_CONFIG_FILES([bin/mesos-local.sh], [chmod +x bin/mesos-local.sh])
AC_CONFIG_FILES([bin/gdb-mesos-master.sh], [chmod +x bin/gdb-mesos-master.sh])
AC_CONFIG_FILES([bin/gdb-mesos-slave.sh], [chmod +x bin/gdb-mesos-slave.sh])
AC_CONFIG_FILES([bin/gdb-mesos-local.sh], [chmod +x bin/gdb-mesos-local.sh])

AC_ARG_ENABLE([java],
              AS_HELP_STRING([--disable-java],
                             [don't build Java bindings]),
              [], [enable_java=yes])

AC_ARG_ENABLE([python],
              AS_HELP_STRING([--disable-python],
                             [don't build Python bindings]),
              [], [enable_python=yes])

AC_ARG_ENABLE([webui],
              AS_HELP_STRING([--disable-webui],
                             [don't build webui (requires Python 2.6)]),
              [], [enable_webui=yes])

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

# Determine the current OS (TODO(benh): Does autotools do this for us?).
case "${target_os}" in
  linux*)
    echo ===========================================================
    echo Setting up build environment for ${target_cpu} ${target_os}
    echo ===========================================================
    OS_NAME=linux
    LIBS="-lrt"
    ;;
  darwin*)
    echo ===========================================================
    echo Setting up build environment for ${target_cpu} ${target_os}
    echo ===========================================================
    OS_NAME=darwin
    CFLAGS="$CFLAGS -D_XOPEN_SOURCE"
    CXXFLAGS="$CXXFLAGS -D_XOPEN_SOURCE"
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
    LIBS="-lsocket -lnsl -lproject -lproc"
    ;;
  *)
    AC_MSG_ERROR("Mesos is currently unsupported on your platform.")
  esac

# Useful for conditionally including source files.
AM_CONDITIONAL([OS_LINUX], [test "x$OS_NAME" = "xlinux"])

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

  AC_SUBST([HAS_JAVA], [1]) # Used to decide whether to run Java tests.
  has_java=yes
fi

AM_CONDITIONAL([HAS_JAVA], [test "x$has_java" = "xyes"])


# Perform necessary configuration for building with Python.
if test "x$enable_python" = "xyes"; then
  AM_PATH_PYTHON() # A fatal error will occur if no interpreter is found.

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
  AC_SUBST([HAS_PYTHON], [1]) # Used to decide whether to run Python tests.
  has_python=yes
fi

AM_CONDITIONAL([HAS_PYTHON], [test "x$has_python" = "xyes"])


# Configure webui as necessary if enabled (must be done *after* we
# configure Python).
if test "x$enable_webui" = "xyes"; then
  # Make sure we have Python.
  test "x$has_python" = "xyes" || \
    AC_MSG_ERROR([webui requires --enable-python])

  # The webui requires at least Python 2.6.
  AM_PYTHON_CHECK_VERSION([$PYTHON], [2.6], [],
                          [AC_MSG_ERROR([webui requires Python >= 2.6])])

  # Determine the flags necessary for embedding Python.
  WEBUI_CPPFLAGS=`$PYTHON -c \
    'from distutils import sysconfig; \
     print "-I" + sysconfig.get_config_var("INCLUDEPY")'`

  WEBUI_LDFLAGS=`$PYTHON -c \
    'from distutils import sysconfig; \
     print sysconfig.get_config_var("LINKFORSHARED") + \
     " -lpython" + sysconfig.get_config_var("VERSION")'`

  AC_SUBST([WEBUI_CPPFLAGS])
  AC_SUBST([WEBUI_LDFLAGS])
  AC_SUBST([WEBUI], [1]) # Used to conditionally include code.
  webui=yes
fi

AM_CONDITIONAL([WEBUI], [test "x$webui" = "xyes"])


# Configure the included ZooKeeper if necessary, if not consider
# checking the installed ZooKeeper version.
if test "x$with_included_zookeeper" = "xyes"; then
  AC_CONFIG_SUBDIRS([third_party/zookeeper-3.3.1/src/c])
fi

AM_CONDITIONAL([WITH_INCLUDED_ZOOKEEPER],
               [test "x$with_included_zookeeper" = "xyes"])


# Checks for programs.
AC_PROG_CXX([g++])
AC_PROG_CC([gcc])

# Check for pthreads (uses m4/acx_pthread.m4).
ACX_PTHREAD([], [AC_MSG_ERROR([failed to find pthreads])])

AC_OUTPUT
