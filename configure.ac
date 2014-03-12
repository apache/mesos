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
AC_INIT([mesos], [0.18.0])

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

# The default CFLAGS/CXXFLAGS from autoconf when using gcc usually
# includes "-O2". These really slow down compiling our tests, so we
# turn them off and enable them (where desired) directly in the
# Makefile. Note that this should not have an impact on users setting
# CFLAGS/CXXFLAGS directly at configure time, or when running make.
AS_IF([test "x${ac_cv_env_CFLAGS_set}" = "x"], [CFLAGS="-g"])
AS_IF([test "x${ac_cv_env_CXXFLAGS_set}" = "x"], [CXXFLAGS="-g"])

# Save the configure arguments so we can pass them to any third-party
# libraries that we might run configure on (see
# 3rdparty/Makefile.am). One downside of our strategy for shipping and
# building third-party libraries is that we can't expose options from
# nested third-party configure scripts.
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

AC_CONFIG_FILES([Makefile])
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
AC_CONFIG_FILES([bin/lldb-mesos-master.sh], [chmod +x bin/lldb-mesos-master.sh])
AC_CONFIG_FILES([bin/lldb-mesos-slave.sh], [chmod +x bin/lldb-mesos-slave.sh])
AC_CONFIG_FILES([bin/lldb-mesos-tests.sh], [chmod +x bin/lldb-mesos-tests.sh])

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

AC_ARG_WITH([included-zookeeper],
            AS_HELP_STRING([--without-included-zookeeper],
                           [excludes building and using the included ZooKeeper
                           package in lieu of a system installed version (note,
                           however, that no attempt is made to find the package
                           and explicitly setting CPPFLAGS and LDFLAGS as
                           appropriate may be necessary)]),
             [], [with_included_zookeeper=yes])

AC_ARG_WITH([cxx11],
            AS_HELP_STRING([--with-cxx11],
                           [builds Mesos using C++11]),
            [], [with_cxx11=no])

# TODO(benh): Support --without-included-protobuf,
# --without-included-glog, etc. Doing this for protobuf is
# considerably more tricky because we need to make sure that 'protoc'
# exists, that a protobuf JAR exists or we can make one, that a
# protobuf egg exists or we can make one, etc.

AC_ARG_VAR([JAVA_HOME], [location of Java Development Kit (JDK)])

AC_ARG_VAR([JAVA_CPPFLAGS], [preprocessor flags for JNI])

AC_ARG_VAR([JAVA_LDFLAGS], [linker flags for JNI])

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
  *)
    AC_MSG_ERROR("Mesos is currently unsupported on your platform.")
esac


# Used for conditionally including source files.
AM_CONDITIONAL([OS_LINUX], [test "x$OS_NAME" = "xlinux"])


# Default to gcc toolchain (we rely on some atomic builtins for now,
# that are also present with clang).
AC_PROG_CXX([g++])
AC_PROG_CC([gcc])


# Check the version of gcc and add any flags as appropriate. Note that
# '-dumpversion' works for clang as well but as of clang 3.3 it
# reports version 4.2.1 (for gcc backwards compatibility).
GCC_VERSION="`${CC} -dumpversion`"
test $? = 0 || AC_MSG_ERROR([failed to determine version of gcc])
case  "${GCC_VERSION}" in
  4.4*)
    echo ===========================================================
    echo Setting up CXXFLAGS for gcc 4.4.*
    echo ===========================================================
    # We fail to build some protobuf generated code with gcc 4.4
    # without setting -fno-strict-aliasing.
    CFLAGS="$CFLAGS -fno-strict-aliasing"
    CXXFLAGS="$CXXFLAGS -fno-strict-aliasing"
  ;;
  4.8*)
    echo ===========================================================
    echo Setting up CXXFLAGS for gcc 4.8.*
    echo ===========================================================
    # Boost 1.53.0 won't compile with GCC 4.8 without -Wno-unused-local-typedefs,
    # and automake doesn't recognize the flag.
    # TODO(brenden): Remove this when Boost has a resolution.
    CFLAGS="${CFLAGS} -Wno-unused-local-typedefs"
    CXXFLAGS="${CXXFLAGS} -Wno-unused-local-typedefs"
  ;;
esac


# Check if we should try and enable optimizations.
if test "x$enable_optimize" = "xyes"; then
  # For now, we only turn on optimizations for gcc.
  if test "x$GCC" = "xyes"; then
    CXXFLAGS="$CXXFLAGS -g2 -O2"
  fi
fi


# Check if we're using clang.
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

# Force C++11 if we're using clang.
if test "x$CLANG" = "xyes"; then
  with_cxx11=yes
fi


# Check for pthreads (uses m4/acx_pthread.m4).
ACX_PTHREAD([], [AC_MSG_ERROR([failed to find pthreads])])


# Check for libunwind, and link it in if present.
AC_CHECK_LIB(unwind, backtrace, LIBS="$LIBS -lunwind")


# TODO(benh): Consider using AS_IF instead of just shell 'if'
# statements for better autoconf style (the AS_IF macros also make
# sure variable dependencies are handled appropriately).

# A helper for checking whether we can compile and link using JNI with
# the current JAVA_CPPFLAGS and JAVA_LDFLAGS.
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
    -o conftest conftest.cpp $JAVA_LDFLAGS >&5

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
  AC_CONFIG_FILES([src/java/mesos.pom])

  AC_DEFINE([MESOS_HAS_JAVA])

  has_java=yes
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
  AC_PYTHON_DEVEL([>= '2.6'])

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
  CXX="$CXX" CC="$CC" CXXFLAGS="$CXXFLAGS" CFLAGS="$CFLAGS" \
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

AC_CHECK_LIB([z], [gzread], [],
             [AC_MSG_ERROR([cannot find libz
-------------------------------------------------------------------
libz is required for mesos to build.
-------------------------------------------------------------------
])])

AC_CHECK_LIB([curl], [curl_global_init], [],
             [AC_MSG_ERROR([cannot find libcurl
-------------------------------------------------------------------
libcurl is required for mesos to build.
-------------------------------------------------------------------
])])


AM_CONDITIONAL([WITH_INCLUDED_ZOOKEEPER],
               [test "x$with_included_zookeeper" = "xyes"])


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


# TODO(benh): Also check for md5 support so we can use the CRAM-MD5
# mechanism. We can likely do a AC_CHECK_LIB looking for a particular
# function only provided if md5 support is present.
AC_CHECK_LIB([sasl2], [sasl_done], [],
             [AC_MSG_ERROR([cannot find libsasl2
-------------------------------------------------------------------
We need libsasl2 for authentication!
-------------------------------------------------------------------
])])


AM_CONDITIONAL([GIT_REPO], [test -d ${srcdir}"/.git"])


AC_OUTPUT
