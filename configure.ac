# Generated with autoscan, then modified appropriately.
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT

# Use explicit python version 2.6.
PYTHON=python2.6

# Update CFLAGS and CXXFLAGS with environment args. 
CFLAGS="-O2 -fno-strict-aliasing -fPIC ${CFLAGS}"
CXXFLAGS="-O2 -fno-strict-aliasing -fPIC ${CXXFLAGS}"

export CFLAGS
export CXXFLAGS

# Update CFLAGS and CXXFLAGS in configure args.
# TODO(benh): Don't send args just for us (e.g., --with-webui).
ac_configure_args=`echo $ac_configure_args | \
                   sed -e "s/CFLAGS=/CFLAGS=-O2 -fno-strict-aliasing -fPIC /"`
ac_configure_args=`echo $ac_configure_args | \
                   sed -e "s/CXXFLAGS=/CXXFLAGS=-O2 -fno-strict-aliasing -fPIC /"`
ac_configure_args="$ac_configure_args --enable-shared=no"

AC_CONFIG_FILES([Makefile src/Makefile src/config.hpp])
AC_CONFIG_SUBDIRS([src/third_party/libprocess])
AC_CONFIG_SUBDIRS([src/third_party/gtest-1.4.0-patched])
AC_CONFIG_SUBDIRS([src/third_party/glog-0.3.0])
AC_CONFIG_SUBDIRS([src/third_party/zookeeper-3.3.1/src/c])

AC_CANONICAL_SYSTEM

UNSUPPORTED_OS="Nexus is currently unsupported on your platform."

case "${target_os}" in
  linux*)
    echo ===========================================================
    echo Setting up build environment for ${target_cpu} ${target_os}
    echo ===========================================================
    OS_NAME=linux
    ;;
  darwin*)
    echo ===========================================================
    echo Setting up build environment for ${target_cpu} ${target_os}
    echo ===========================================================
    OS_NAME=darwin
    CFLAGS="${CFLAGS} -D_XOPEN_SOURCE"
    CXXFLAGS="${CXXFLAGS} -D_XOPEN_SOURCE"
    LDFLAGS="${LDFLAGS} `$PYTHON -c 'from distutils import sysconfig; print sysconfig.get_config_var("LINKFORSHARED") + " -lpython" + sysconfig.get_config_var("VERSION")'`"
    ;;
  solaris*)
    echo ===========================================================
    echo Setting up build environment for ${target_cpu} ${target_os}
    echo ===========================================================
    CC=gcc-4.3.2
    CXX=g++-4.3.2
    export CC
    export CXX
    CFLAGS="${CFLAGS} -pthreads -march=i586"
    CXXFLAGS="${CXXFLAGS} -pthreads -march=i586"
    LIBS="-lsocket -lnsl -lproject -lproc"
    PYTHON="python2.6"
    OS_NAME=solaris
    ;;
  *)
    AC_MSG_ERROR($UNSUPPORTED_OS)
  esac

# Export OS name to Makefiles
AC_SUBST(OS_NAME)

JAVA_HOME=""
AC_ARG_WITH([java-home],
  AC_HELP_STRING([--with-java-home=DIR],
                 [find Java Development Kit in DIR]),
	         [case "$with_java_home" in
		    yes) AC_MSG_ERROR([
*** --with-java-home requires an argument]) ;;
		    *) JAVA_HOME="$withval" ;;
	          esac],
	         [])
AC_SUBST(JAVA_HOME)

PYTHON_HEADERS=""
AC_ARG_WITH([python-headers],
  AC_HELP_STRING([--with-python-headers=DIR],
                 [find Python headers in DIR]),
	         [case "$with_python_headers" in
		    yes) AC_MSG_ERROR([
*** --with-python-headers requires an argument]) ;;
		    *) PYTHON_HEADERS="$withval" ;;
	          esac],
	         [])
AC_SUBST(PYTHON_HEADERS)

RUBY_HEADERS=""
AC_ARG_WITH([ruby-headers],
  AC_HELP_STRING([--with-ruby-headers=DIR],
                 [find Ruby headers in DIR]),
	         [case "$with_ruby_headers" in
		    yes) AC_MSG_ERROR([
*** --with-ruby-headers requires an argument]) ;;
		    *) RUBY_HEADERS="$withval" ;;
	          esac],
	         [])
AC_SUBST(RUBY_HEADERS)

WITH_WEBUI="0"
WEBUI_FLAGS=""
AC_ARG_WITH([webui],
  AC_HELP_STRING([--with-webui],
                 [enable web UI (requires Python)]),
	         [case "$with_webui" in
		    yes) WITH_WEBUI="1"
                         echo running $PYTHON to find compiler flags for embedding it...
                         WEBUI_FLAGS=`$PYTHON -c 'from distutils import sysconfig; print sysconfig.get_config_var("LINKFORSHARED") + " -lpython" + sysconfig.get_config_var("VERSION") + " -I" + sysconfig.get_config_var("INCLUDEPY")'`
                         if [[ "$?" -ne "0" ]]; then
                           AC_MSG_ERROR([
*** Failed to determine compiler flags for embedding Python, needed by web UI.
*** Check that you have Python 2.6 development packages installed.])
                         fi ;;
                    no) WITH_WEBUI="0" ;;
		    *) AC_MSG_ERROR([
*** --with-webui does not take arguments]) ;;
	          esac],
	         [])
AC_SUBST(WITH_WEBUI)
AC_SUBST(WEBUI_FLAGS)

LOCAL_ZOOKEEPER="no"
AC_ARG_WITH([local-zookeeper],
  AC_HELP_STRING([--with-local-zookeeper],
                 [Use local ZooKeeper for building]),
	         [case "$with_local_zookeeper" in
		    yes) LOCAL_ZOOKEEPER="yes" ;;
                    no) LOCAL_ZOOKEEPER="no" ;;
		    *) AC_MSG_ERROR([
*** --with-local-zookeeper does not take arguments]) ;;
	          esac],
	         [])
AC_SUBST(LOCAL_ZOOKEEPER)

# Checks for programs.
AC_PROG_CXX([g++])
AC_PROG_CC([gcc])

AC_OUTPUT
