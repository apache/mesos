---
title: Apache Mesos - CMake Options
layout: documentation
---

# CMake Options

*The most up-to-date options can be found with `cmake .. -LAH`.*

See more information in the [CMake documentation](../cmake.md).

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
      -DVERBOSE=(TRUE|FALSE)
    </td>
    <td>
      Generate a build solution that produces verbose output
      (for example, verbose Makefiles). [default=TRUE]
    </td>
  </tr>
  <tr>
    <td>
      -DBUILD_SHARED_LIBS=(TRUE|FALSE)
    </td>
    <td>
      Build shared libraries (where possible).
      [default=FALSE for Windows, TRUE otherwise]
    </td>
  </tr>
  <tr>
    <td>
      -DENABLE_GC_UNUSED=(TRUE|FALSE)
    </td>
    <td>
      Enable garbage collection of unused program segments. This option
      significantly reduces the size of the final build artifacts.  [default=FALSE]
    </td>
  </tr>
  <tr>
    <td>
      -DENABLE_PRECOMPILED_HEADERS=(TRUE|FALSE)
    </td>
    <td>
      Enable auto-generated precompiled headers using cotire.
      [default=TRUE for Windows, FALSE otherwise]
    </td>
  </tr>
  <tr>
    <td>
      -DCPACK_BINARY_[TYPE]=(TRUE|FALSE)
    </td>
    <td>
      Where [TYPE] is one of BUNDLE, DEB, DRAGNDROP, IFW, NSIS, OSXX11,
      PACKAGEMAKER, RPM, STGZ, TBZ2, TGZ, TXZ. This modifies the 'package'
      target to generate binary package of the specified format. A binary
      package contains everything that would be installed via CMake's 'install'
      target. [default=FALSE]
    </td>
  </tr>
  <tr>
    <td>
      -DCPACK_SOURCE_[TYPE]=(TRUE|FALSE)
    </td>
    <td>
      Where [TYPE] is one of TBZ2, TXZ, TZ, ZIP. This modifies the
      'package_source' target to generate a package of the sources required to
      build and test Mesos, in the specified format. [default=FALSE]
    </td>
  </tr>
  <tr>
    <td>
      -DREBUNDLED=(TRUE|FALSE)
    </td>
    <td>
      Attempt to build against the third-party dependencies included as tarballs
      in the Mesos repository. NOTE: This is not always possible. For example, a
      dependency might not be included as a tarball in the Mesos repository;
      additionally, Windows does not have a package manager, so we do not expect
      system dependencies like APR to exist natively, and we therefore must
      acquire them. In these cases (or when <code>-DREBUNDLED=FALSE</code>), we
      will acquire the dependency from the location specified by the
      <code>3RDPARTY_DEPENDENCIES</code>, which by default points to the
      official Mesos <a
      href="https://github.com/mesos/3rdparty">third-party dependency
      mirror</a>. [default=TRUE]
    </td>
  </tr>
  <tr>
    <td>
      -DENABLE_LIBEVENT=(TRUE|FALSE)
    </td>
    <td>
      Use <a href="https://github.com/libevent/libevent">libevent</a> instead of
      libev for the event loop. This is required (but not the default) on
      Windows. [default=FALSE]
    </td>
  </tr>
  <tr>
    <td>
      -DUNBUNDLED_LIBEVENT=(TRUE|FALSE)
    </td>
    <td>
      Build libprocess with an installed libevent version instead of the bundled.
      [default=TRUE for macOS, FALSE otherwise]
    </td>
  </tr>
  <tr>
    <td>
      -DLIBEVENT_ROOT_DIR=[path]
    </td>
    <td>
      Specify the path to libevent, e.g. "C:\libevent-Win64".
      [default=unspecified]
    </td>
  </tr>
  <tr>
    <td>
      -DENABLE_SSL=(TRUE|FALSE)
    </td>
    <td>
      Build libprocess with SSL support. [default=FALSE]
    </td>
  </tr>
  <tr>
    <td>
      -DOPENSSL_ROOT_DIR=[path]
    </td>
    <td>
      Specify the path to OpenSSL, e.g. "C:\OpenSSL-Win64".
      [default=unspecified]
    </td>
  </tr>
  <tr>
    <td>
      -DENABLE_LOCK_FREE_RUN_QUEUE=(TRUE|FALSE)
    </td>
    <td>
      Build libprocess with lock free run queue. [default=FALSE]
    </td>
  </tr>
  <tr>
    <td>
      -DENABLE_JAVA=(TRUE|FALSE)
    </td>
    <td>
      Build Java components. Warning: this is SLOW. [default=FALSE]
    </td>
  </tr>
  <tr>
    <td>
      -DENABLE_NEW_CLI=(TRUE|FALSE)
    </td>
    <td>
      Enable the new Python CLI by building a binary using PyInstaller. This option
      requires Python 3 which can be set using the CMake option. [default=FALSE]
    </td>
  </tr>
  <tr>
    <td>
      -DPYTHON_3=[path]
    </td>
    <td>
      Specify the path to Python 3, e.g. "python36".
      [default=unspecified]
    </td>
  </tr>
  <tr>
    <td>
      -D3RDPARTY_DEPENDENCIES=[path_or_url]
    </td>
    <td>
      Location of the dependency mirror. In some cases, the Mesos build system
      needs to acquire third-party dependencies that aren't rebundled as
      tarballs in the Mesos repository. For example, on Windows, we must acquire
      newer versions of some dependencies, and since Windows does not have a
      package manager, we must acquire system dependencies like cURL. This
      parameter can be either a URL (for example, pointing at the Mesos official
      <a href="https://github.com/mesos/3rdparty">third-party
      dependency mirror</a>), or a local folder (for example, a local clone of
      the dependency mirror).
      [default="https://github.com/mesos/3rdparty/raw/master"]
    </td>
  </tr>
  <tr>
    <td>
      -DPATCHEXE_PATH=[path]
    </td>
    <td>
      Location of
      <a href="http://gnuwin32.sourceforge.net/packages/patch.htm">GNU Patch for
      Windows</a> binary. [default=%PROGRAMFILESX86%/GnuWin32/bin/patch.exe]
      </td>
  </tr>
  <tr>
    <td>
      -DENABLE_NVML=(TRUE|FALSE)
    </td>
    <td>
      Enable use of the NVML headers. [default=TRUE]
    </td>
  </tr>
  <tr>
    <td>
      -DMESOS_FINAL_PREFIX=[path]
    </td>
    <td>
      Adjust built-in paths (rpath in shared objects, default paths in Mesos
      flags and so on) so that cmake install output works after being copied
      into this prefix. This path does not have to exist on the build system
      (the system where cmake install is invoked).
      This option is typically used by package managers that use different
      prefixes on a build system and on a target system.
      [default=`${CMAKE_INSTALL_PREFIX}`]
    </td>
  </tr>
  <tr>
    <td>
      -DMESOS_INSTALL_TESTS=(TRUE|FALSE)
    </td>
    <td>
      Add test executables and their dependencies to the install output.
    </td>
  </tr>
</table>
