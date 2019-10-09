---
title: Apache Mesos - Autotools Options
layout: documentation
---

# Autotools Options

*The most up-to-date options can be found with `./configure --help`.*

## Autotools `configure` script options

<table class="table table-striped">
  <thead>
    <tr>
      <th width="30%">
        Flag
      </th>
      <th>
        Explanation
      </th>
    </tr>
  </thead>
  <tr>
    <td>
      --enable-static[=PKGS]
    </td>
    <td>
      Build static libraries. [default=yes]
    </td>
  </tr>
  <tr>
    <td>
      --enable-dependency-tracking
    </td>
    <td>
      Do not reject slow dependency extractors.
    </td>
  </tr>
  <tr>
    <td>
      --disable-dependency-tracking
    </td>
    <td>
      Speeds up one-time build.
    </td>
  </tr>
  <tr>
    <td>
      --enable-silent-rules
    </td>
    <td>
      Less verbose build output (undo: "make V=1").
    </td>
  </tr>
  <tr>
    <td>
      --disable-silent-rules
    </td>
    <td>
      Verbose build output (undo: "make V=0").
    </td>
  </tr>
  <tr>
    <td>
      --disable-maintainer-mode
    </td>
    <td>
      Disable make rules and dependencies not useful (and sometimes confusing)
      to the casual installer.
    </td>
  </tr>
  <tr>
    <td>
      --enable-shared[=PKGS]
    </td>
    <td>
      Build shared libraries. [default=yes]
    </td>
  </tr>
  <tr>
    <td>
      --enable-fast-install[=PKGS]
    </td>
    <td>
      Optimize for fast installation. [default=yes]
    </td>
  </tr>
  <tr>
    <td>
      --enable-gc-unused
    </td>
    <td>
      Enable garbage collection of unused program segments. This option
      significantly reduces the size of the final build artifacts. [default=no]
    </td>
  </tr>
  <tr>
    <td>
      --disable-libtool-lock
    </td>
    <td>
      Avoid locking. Note that this might break parallel builds.
    </td>
  </tr>
  <tr>
    <td>
      --disable-bundled
    </td>
    <td>
      Configures Mesos to build against preinstalled dependencies
      instead of bundled libraries.
    </td>
  </tr>
  <tr>
    <td>
      --disable-bundled-pip
    </td>
    <td>
      Excludes building and using the bundled pip package in lieu of an
      installed version in <code>PYTHONPATH</code>.
    </td>
  </tr>
  <tr>
    <td>
      --disable-bundled-setuptools
    </td>
    <td>
      Excludes building and using the bundled setuptools package in lieu of an
      installed version in <code>PYTHONPATH</code>.
    </td>
  </tr>
  <tr>
    <td>
      --disable-bundled-wheel
    </td>
    <td>
      Excludes building and using the bundled wheel package in lieu of an
      installed version in <code>PYTHONPATH</code>.
    </td>
  </tr>
  <tr>
    <td>
      --enable-debug
    </td>
    <td>
      Whether debugging is enabled. If CFLAGS/CXXFLAGS are set, this
      option won't change them. [default=no]
    </td>
  </tr>
  <tr>
    <td>
      --disable-java
    </td>
    <td>
      Don't build Java bindings.
    </td>
  </tr>
  <tr>
    <td>
      --enable-libevent
    </td>
    <td>
      Use <a href="https://github.com/libevent/libevent">libevent</a>
      instead of libev for the libprocess event loop. Note that the libevent
      version 2+ development package is required. [default=no]
    </td>
  </tr>
  <tr>
    <td>
      --enable-install-module-dependencies
    </td>
    <td>
      Install third-party bundled dependencies required for module development.
      [default=no]
    </td>
  </tr>
  <tr>
    <td>
      --enable-optimize
    </td>
    <td>
      Whether optimizations are enabled. If CFLAGS/CXXFLAGS are set,
      this option won't change them. [default=no]
    </td>
  </tr>
  <tr>
    <td>
      --enable-perftools
    </td>
    <td>
      Whether profiling with Google perftools is enabled. [default=no]
    </td>
  </tr>
  <tr>
    <td>
      --enable-parallel-test-execution
    </td>
    <td>
      Whether to attempt to run tests in parallel.
    </td>
  </tr>
  <tr>
    <td>
      --enable-new-cli
    </td>
    <td>
      Whether to build the new Python CLI. This option requires Python 3
      which can be set using the PYTHON_3 environment variable.
      [default=no]
    </td>
  </tr>
  <tr>
    <td>
      --disable-python
    </td>
    <td>
      Don't build Python bindings.
    </td>
  </tr>
  <tr>
    <td>
      --disable-python-dependency-install
    </td>
    <td>
      When the python packages are installed during make install, no external
      dependencies will be downloaded or installed.
    </td>
  </tr>
  <tr>
    <td>
      --enable-ssl
    </td>
    <td>
      Enable <a href="/documentation/latest/ssl">SSL</a> for libprocess
      communication. [default=no]
    </td>
  </tr>
  <tr>
    <td>
      --enable-static-unimplemented
    </td>
    <td>
      Generate static assertion errors for unimplemented functions. [default=no]
    </td>
  </tr>
  <tr>
    <td>
      --enable-tests-install
    </td>
    <td>
      Build and install tests and their helper tools. [default=no]
    </td>
  </tr>
  <tr>
    <td>
      --enable-xfs-disk-isolator
    </td>
    <td>
      Builds the XFS disk isolator. [default=no]
    </td>
  </tr>
  <tr>
    <td>
      --disable-zlib
    </td>
    <td>
      Disables zlib compression, which means the webui will be far less
      responsive; not recommended.
    </td>
  </tr>
  <tr>
    <td>
      --enable-lock-free-event-queue
    </td>
    <td>
      Enables the lock-free event queue to be used in libprocess which
      greatly improves message passing performance!
    </td>
  </tr>
  <tr>
    <td>
      --disable-werror
    </td>
    <td>
      Disables treating compiler warnings as fatal errors.
    </td>
  </tr>
</table>

### Autotools `configure` script optional package flags

<table class="table table-striped">
  <thead>
    <tr>
      <th width="30%">
        Flag
      </th>
      <th>
        Explanation
      </th>
    </tr>
  </thead>
  <tr>
    <td>
      --with-gnu-ld
    </td>
    <td>
      Assume the C compiler uses GNU <code>ld</code>. [default=no]
    </td>
  </tr>
  <tr>
    <td>
      --with-sysroot[=DIR]
    </td>
    <td>
      Search for dependent libraries within <code>DIR</code>
      (or the compiler's sysroot if not specified).
    </td>
  </tr>
  <tr>
    <td>
      --with-apr[=DIR]
    </td>
    <td>
      Specify where to locate the apr-1 library.
    </td>
  </tr>
  <tr>
    <td>
      --with-boost[=DIR]
    </td>
    <td>
      Excludes building and using the bundled Boost package in lieu of an
      installed version at a location prefixed by the given path.
    </td>
  </tr>
  <tr>
    <td>
      --with-concurrentqueue[=DIR]
    </td>
    <td>
      Excludes building and using the bundled concurrentqueue package in lieu
      of an installed version at a location prefixed by the given path.
    </td>
  </tr>
  <tr>
    <td>
      --with-curl[=DIR]
    </td>
    <td>
      Specify where to locate the curl library.
    </td>
  </tr>
  <tr>
    <td>
      --with-elfio[=DIR]
    </td>
    <td>
      Excludes building and using the bundled ELFIO package in lieu of an
      installed version at a location prefixed by the given path.
    </td>
  </tr>
  <tr>
    <td>
      --with-glog[=DIR]
    </td>
    <td>
      excludes building and using the bundled glog package in lieu of an
      installed version at a location prefixed by the given path.
    </td>
  </tr>
  <tr>
    <td>
      --with-gmock[=DIR]
    </td>
    <td>
      Excludes building and using the bundled gmock package in lieu of an
      installed version at a location prefixed by the given path.
    </td>
  </tr>
  <tr>
    <td>
      --with-http-parser[=DIR]
    </td>
    <td>
      Excludes building and using the bundled http-parser package in lieu of an
      installed version at a location prefixed by the given path.
    </td>
  </tr>
  <tr>
    <td>
      --with-leveldb[=DIR]
    </td>
    <td>
      Excludes building and using the bundled LevelDB package in lieu of an
      installed version at a location prefixed by the given path.
    </td>
  </tr>
  <tr>
    <td>
      --with-libev[=DIR]
    </td>
    <td>
      Excludes building and using the bundled libev package in lieu of an
      installed version at a location prefixed by the given path.
    </td>
  </tr>
  <tr>
    <td>
      --with-libevent[=DIR]
    </td>
    <td>
      Specify where to locate the libevent library.
    </td>
  </tr>
  <tr>
    <td>
      --with-libprocess[=DIR]
    </td>
    <td>
      Specify where to locate the libprocess library.
    </td>
  </tr>
  <tr>
    <td>
      --with-network-isolator
    </td>
    <td>
      Builds the network isolator.
    </td>
  </tr>
  <tr>
    <td>
      --with-nl[=DIR]
    </td>
    <td>
      Specify where to locate the
      <a href="https://www.infradead.org/~tgr/libnl/">libnl3</a> library,
      which is required for the network isolator.
    </td>
  </tr>
  <tr>
    <td>
      --with-nvml[=DIR]
    </td>
    <td>
      Excludes building and using the bundled NVML headers in lieu of an
      installed version at a location prefixed by the given path.
    </td>
  </tr>
  <tr>
    <td>
      --with-picojson[=DIR]
    </td>
    <td>
      Excludes building and using the bundled picojson package in lieu of an
      installed version at a location prefixed by the given path.
    </td>
  </tr>
  <tr>
    <td>
      --with-protobuf[=DIR]
    </td>
    <td>
      Excludes building and using the bundled protobuf package in lieu of an
      installed version at a location prefixed by the given path.
    </td>
  </tr>
  <tr>
    <td>
      --with-sasl[=DIR]
    </td>
    <td>
      Specify where to locate the sasl2 library.
    </td>
  </tr>
  <tr>
    <td>
      --with-ssl[=DIR]
    </td>
    <td>
      Specify where to locate the ssl library.
    </td>
  </tr>
  <tr>
    <td>
      --with-stout[=DIR]
    </td>
    <td>
      Specify where to locate stout library.
    </td>
  </tr>
  <tr>
    <td>
      --with-svn[=DIR]
    </td>
    <td>
      Specify where to locate the svn-1 library.
    </td>
  </tr>
  <tr>
    <td>
      --with-zlib[=DIR]
    </td>
    <td>
      Specify where to locate the zlib library.
    </td>
  </tr>
  <tr>
    <td>
      --with-zookeeper[=DIR]
    </td>
    <td>
      Excludes building and using the bundled ZooKeeper package in lieu of an
      installed version at a location prefixed by the given path.
    </td>
  </tr>
</table>

### Environment variables which affect the Autotools `configure` script

Use these variables to override the choices made by `configure` or to help
it to find libraries and programs with nonstandard names/locations.

<table class="table table-striped">
  <thead>
    <tr>
      <th width="30%">
        Variable
      </th>
      <th>
        Explanation
      </th>
    </tr>
  </thead>
  <tr>
    <td>
      JAVA_HOME
    </td>
    <td>
      Location of Java Development Kit (JDK).
    </td>
  </tr>
  <tr>
    <td>
      JAVA_CPPFLAGS
    </td>
    <td>
      Preprocessor flags for JNI.
    </td>
  </tr>
  <tr>
    <td>
      JAVA_JVM_LIBRARY
    </td>
    <td>
      Full path to <code>libjvm.so</code>.
    </td>
  </tr>
  <tr>
    <td>
      MAVEN_HOME
    </td>
    <td>
      Looks for <code>mvn</code> at <code>MAVEN_HOME/bin/mvn</code>.
    </td>
  </tr>
  <tr>
    <td>
      PROTOBUF_JAR
    </td>
    <td>
      Full path to protobuf jar on prefixed builds.
    </td>
  </tr>
  <tr>
    <td>
      PYTHON
    </td>
    <td>
      Which Python 2 interpreter to use.
    </td>
  </tr>
  <tr>
    <td>
      PYTHON_VERSION
    </td>
    <td>
      The installed Python 2 version to use, for example '2.3'. This string will
      be appended to the Python 2 interpreter canonical name.
    </td>
  </tr>
  <tr>
    <td>
      PYTHON_3
    </td>
    <td>
      Which Python 3 interpreter to use.
    </td>
  </tr>
  <tr>
    <td>
      PYTHON_3_VERSION
    </td>
    <td>
      The installed Python 3 version to use, for example '3.6'. This string will
      be appended to the Python 3 interpreter canonical name.
    </td>
  </tr>
</table>
