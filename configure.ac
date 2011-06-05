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
                   sed -e "s/CFLAGS=/CFLAGS=-O2 -fno-strict-aliasing /"`
ac_configure_args=`echo $ac_configure_args | \
                   sed -e "s/CXXFLAGS=/CXXFLAGS=-O2 -fno-strict-aliasing /"`
ac_configure_args="$ac_configure_args --enable-shared=no --with-pic"

AC_CONFIG_FILES([Makefile
                 src/Makefile
                 src/examples/Makefile
                 src/examples/java/Makefile
                 src/examples/python/Makefile
                 src/tests/Makefile
                 src/config/config.hpp])
AC_CONFIG_SUBDIRS([third_party/libprocess])
AC_CONFIG_SUBDIRS([third_party/glog-0.3.1])
AC_CONFIG_SUBDIRS([third_party/gmock-1.5.0])
AC_CONFIG_SUBDIRS([third_party/zookeeper-3.3.1/src/c])

AC_CANONICAL_SYSTEM

UNSUPPORTED_OS="Mesos is currently unsupported on your platform."

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

JAVA_HEADERS=""
AC_ARG_WITH([java-headers],
  AC_HELP_STRING([--with-java-headers=DIR],
                 [find Java headers in DIR (if different from JAVA_HOME/include)]),
	         [case "$with_java_headers" in
		    yes) 
                      AC_MSG_ERROR([
*** --with-java-headers requires an argument]) ;;
		    *)
                      JAVA_HEADERS="$withval"
                      ;;
	          esac],
	         [])
AC_SUBST(JAVA_HEADERS)

JAVA_HOME=""
HAS_JAVA="0"
AC_ARG_WITH([java-home],
  AC_HELP_STRING([--with-java-home=DIR],
                 [find Java Development Kit in DIR]),
	         [case "$with_java_home" in
		    yes) 
                      AC_MSG_ERROR([
*** --with-java-home requires an argument]) ;;
		    *)
                      JAVA_HOME="$withval"
                      HAS_JAVA="1"
                      if test -z "$JAVA_HEADERS"; then
                        JAVA_HEADERS="$JAVA_HOME/include/$OS_NAME"
                      fi
                      ;;
	          esac],
	         [])
AC_SUBST(JAVA_HOME)
AC_SUBST(HAS_JAVA)

PYTHON_HEADERS=""
PYTHON_LDFLAGS=""
HAS_PYTHON="0"
AC_ARG_WITH([python-headers],
  AC_HELP_STRING([--with-python-headers=DIR],
                 [find Python headers in DIR]),
	         [case "$with_python_headers" in
		    yes)
                      AC_MSG_ERROR([
*** --with-python-headers requires an argument]) ;;
		    *)
                      PYTHON_HEADERS="$withval"
                      echo running $PYTHON to find compiler flags for creating the Mesos Python library...
                      PYTHON_LDFLAGS=`$PYTHON -c 'from distutils import sysconfig; print sysconfig.get_config_var("LINKFORSHARED") + " -lpython" + sysconfig.get_config_var("VERSION")'`
                      HAS_PYTHON="1"
                      ;;
	          esac],
	         [])
AC_SUBST(PYTHON_HEADERS)
AC_SUBST(PYTHON_LDFLAGS)
AC_SUBST(HAS_PYTHON)

WITH_WEBUI="0"
WEBUI_CFLAGS=""
WEBUI_CXXFLAGS=""
WEBUI_LDFLAGS=""
AC_ARG_WITH([webui],
  AC_HELP_STRING([--with-webui],
                 [enable web UI (requires Python)]),
	         [case "$with_webui" in
		    yes) WITH_WEBUI="1"
                         echo running $PYTHON to find compiler flags for embedding it...
                         WEBUI_CFLAGS=`$PYTHON -c 'from distutils import sysconfig; print "-I" + sysconfig.get_config_var("INCLUDEPY")'`
                         WEBUI_CXXFLAGS=$WEBUI_CFLAGS
                         WEBUI_LDFLAGS=`$PYTHON -c 'from distutils import sysconfig; print sysconfig.get_config_var("LINKFORSHARED") + " -lpython" + sysconfig.get_config_var("VERSION")'`
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
AC_SUBST(WEBUI_CFLAGS)
AC_SUBST(WEBUI_CXXFLAGS)
AC_SUBST(WEBUI_LDFLAGS)

SWIG="swig"
AC_ARG_WITH([swig],
  AC_HELP_STRING([--with-swig=PATH],
                 [use the SWIG binary at PATH]),
	         [case "$with_swig" in
		    yes) AC_MSG_ERROR([
*** --with-swig requires an argument]) ;;
		    *) SWIG="$withval" ;;
	          esac],
	         [])
AC_SUBST(SWIG)

WITH_ZOOKEEPER="0"
AC_ARG_WITH([zookeeper],
  AC_HELP_STRING([--with-zookeeper],
                 [use ZooKeeper support]),
	         [case "$with_zookeeper" in
		    yes) WITH_ZOOKEEPER="1" ;;
                    no) WITH_ZOOKEEPER="0" ;;
		    *) AC_MSG_ERROR([
*** --with-zookeeper does not take arguments]) ;;
	          esac],
	         [])
AC_SUBST(WITH_ZOOKEEPER)

WITH_INCLUDED_ZOOKEEPER="0"
AC_ARG_WITH([included-zookeeper],
  AC_HELP_STRING([--with-included-zookeeper],
                 [build with included ZooKeeper]),
	         [case "$with_included_zookeeper" in
		    yes) WITH_ZOOKEEPER="1"
                         WITH_INCLUDED_ZOOKEEPER="1" ;;
                    no) WITH_INCLUDED_ZOOKEEPER="0" ;;
		    *) AC_MSG_ERROR([
*** --with-included-zookeeper does not take arguments]) ;;
	          esac],
	         [])
AC_SUBST(WITH_INCLUDED_ZOOKEEPER)

# Checks for programs.
AC_PROG_CXX([g++])
AC_PROG_CC([gcc])

AC_OUTPUT
