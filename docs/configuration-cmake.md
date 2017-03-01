---
title: Apache Mesos - CMake Configuration
layout: documentation
---

# Mesos CMake Build Configuration Options

Mesos currently exposes two build systems, one written in
[autotools](configuration.md), and one written in CMake.
This document describes the configuration flags available in
the CMake build system.


## CMake configuration options

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
      -D3RDPARTY_DEPENDENCIES[=path_or_url]
    </td>
    <td>
      Location of the dependency mirror. In some cases, the Mesos build system
      needs to acquire third-party dependencies that aren't rebundled as
      tarballs in the Mesos repository. For example, on Windows, we must aquire
      newer versions of some dependencies, and since Windows does not have a
      package manager, we must acquire system dependencies like cURL. This
      parameter can be either a URL (for example, pointing at the Mesos official
      [third-party dependency mirror](https://github.com/3rdparty/mesos-3rdparty)),
      or a local folder (for example, a local clone of the dependency mirror).
      [default=https://github.com/3rdparty/mesos-3rdparty]
    </td>
  </tr>
  <tr>
    <td>
      -DCPACK_BINARY_(BUNDLE|DEB|DRAGNDROP|IFW|NSIS|OSXX11|PACKAGEMAKER|RPM|STGZ|TBZ2|TGZ|TXZ)
    </td>
    <td>
      This modifies the 'package' target to generate binary package of
      the specified format.  A binary package contains everything that
      would be installed via CMake's 'install' target.
      [default=OFF]
    </td>
  </tr>
  <tr>
    <td>
      -DCPACK_SOURCE_(TBZ2|TXZ|TZ|ZIP)
    </td>
    <td>
      This modifies the 'package_source' target to generate a package of the
      sources required to build and test Mesos, in the specified format.
      [default=OFF]
    </td>
  </tr>
  <tr>
    <td>
      -DENABLE_LIBEVENT
    </td>
    <td>
      Use libevent instead of libev for the event loop. [default=FALSE]
    </td>
  </tr>
  <tr>
    <td>
      -DENABLE_SSL
    </td>
    <td>
      Build libprocess with SSL support. [default=FALSE]
    </td>
  </tr>
  <tr>
    <td>
      -DHAS_AUTHENTICATION
    </td>
    <td>
      Build Mesos with support for authentication. [default=TRUE]
    </td>
  </tr>
  <tr>
    <td>
      -DREBUNDLED
    </td>
    <td>
      Attempt to build against the third-party dependencies included as
      tarballs in the Mesos repository.

      NOTE: This is not always possible.  For example, a dependency might
      not be included as a tarball in the Mesos repository; additionally,
      Windows does not have a package manager, so we do not expect system
      dependencies like APR to exist natively, and we therefore must acquire
      them. In these cases (or when `REBUNDLED` is set to `FALSE`), we will
      acquire the dependency from the location specified by the
      `3RDPARTY_DEPENDENCIES`, which by default points to the official Mesos
      [third-party dependency mirror](https://github.com/3rdparty/mesos-3rdparty).
      [default=TRUE]
    </td>
  </tr>
  <tr>
    <td>
      -DVERBOSE
    </td>
    <td>
      Generate a build solution that produces verbose output
      (for example, verbose Makefiles). [default=TRUE]
    </td>
  </tr>
</table>
