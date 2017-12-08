---
title: Apache Mesos - Windows
layout: documentation
---

# Windows

Mesos 1.0.0 introduced experimental support for Windows.


## Building Mesos


### System Requirements

1. Install the latest [Visual Studio 2017](https://www.visualstudio.com/downloads/):
   The "Community" edition is sufficient (and free).
   During installation, choose the "Desktop development with C++" workload.

2. Install [CMake 3.8.0](https://cmake.org/download/) or later.
   During installation, choose to "Add CMake to the system PATH for all users".

3. Install [GNU patch for Windows](http://gnuwin32.sourceforge.net/packages/patch.htm).

4. If building from source, install [Git](https://git-scm.com/download/win).
   During installation, keep the defaults to "Use Git from the Windows
   Command Prompt", and "Checkout Windows-style, commit Unix-style
   line endings" (i.e. `git config core.autocrlf true`).

5. Make sure there are no spaces in your build directory.
   For example, `C:/Program Files (x86)/mesos` is an invalid build directory.

6. If developing Mesos, install [Python 2](https://www.python.org/downloads/)
   (not Python 3), in order to use our support scripts (e.g. to post and apply
   patches, or lint source code).


### Build Instructions

Following are the instructions for Windows 10.

    # Clone (or extract) Mesos.
    git clone https://git-wip-us.apache.org/repos/asf/mesos.git
    cd mesos

    # Configure using CMake for an out-of-tree build.
    mkdir build
    cd build
    cmake .. -G "Visual Studio 15 2017 Win64" -T "host=x64" -DENABLE_LIBEVENT=1

    # Build Mesos.
    # To build just the Mesos agent, add `--target mesos-agent`.
    cmake --build .

    # The Windows agent exposes new isolators that must be used as with
    # the `--isolation` flag. To get started point the agent to a working
    # master, using eiher an IP address or zookeeper information.
    src\mesos-agent.exe --master=<master> --work_dir=<work folder> --launcher_dir=<repository>\build\src


## Known Limitations

The current implementation is known to have the following limitations:

* Only the agent should be run on Windows.  The Mesos master can be
  launched, but only for testing as the master does not support
  high-availability setups on Windows.

* While Mesos supports NTFS long paths internally, tasks which do not support
  long paths must be run on agent whose `--work_dir` is a short path.

* The minimum versions of Windows supported are: Windows 10 Creators Update (AKA
  version 1703, build number 15063), and [Windows Server, version 1709][server].
  It is likely that this will increase, due to evolving Windows container
  support and developer features which ease porting.

[server]: https://docs.microsoft.com/en-us/windows-server/get-started/get-started-with-1709


## Status

For more information regarding the status of Windows support in Mesos,
please refer to the [JIRA epic](https://issues.apache.org/jira/browse/MESOS-3094).

## Build Configuration Examples

### Building with Java

This enables more unit tests, but we do not yet officially produce
`mesos-master`.

When building with Java on Windows, you must add the [Maven][] build tool to
your path. The `JAVA_HOME` environment variable must also be manually set.
An installation of the Java SDK can be found form [Oracle][].

[maven]: https://maven.apache.org/guides/getting-started/windows-prerequisites.html
[oracle]: http://www.oracle.com/technetwork/java/javase/downloads/index.html

As of this writing, Java 9 is not yet supported, but Java 8 has been tested.

The Java build defaults to `OFF` because it is slow. To build the Java
components on Windows, turn it `ON`:

```powershell
mkdir build; cd build
$env:PATH += ";C:\...\apache-maven-3.3.9\bin\"
$env:JAVA_HOME = "C:\Program Files\Java\jdk1.8.0_144"
cmake .. -DENABLE_JAVA=ON -DENABLE_LIBEVENT=ON -G "Visual Studio 15 2017 Win64" -T "host=x64"
cmake --build . --target mesos-java
```

Note that the `mesos-java` library does not have to be manually built; as
`libmesos` will link it when Java is enabled.

Unfortunately, on Windows the `FindJNI` CMake module will populate `JAVA_JVM_LIBRARY` with
the path to the static `jvm.lib`, but this variable must point to the shared
library, `jvm.dll`, as it is loaded at runtime. Set it correctly like this:

```
$env:JAVA_JVM_LIBRARY = "C:\Program Files\Java\jdk1.8.0_144\jre\bin\server\jvm.dll"
```

The library may still fail to load at runtime with the following error:

> "The specified module could not be found."

If this is the case, and the path to `jvm.dll` is verified to be correct, then
the error message actually indicates that the dependencies of `jvm.dll` could
not be found. On Windows, the DLL search path includes the environment variable
`PATH`, so add the `bin` folder which contains `server\jvm.dll` to `PATH`:

```
$env:PATH += ";C:\Program Files\Java\jdk1.8.0_144\jre\bin"
```

### Building with OpenSSL

When building with OpenSSL on Windows, you must build or install a distribution
of OpenSSL for Windows. A commonly chosen distribution is
[Shining Light Productions' OpenSSL][openssl].

[openssl]: https://slproweb.com/products/Win32OpenSSL.html

As of this writing, OpenSSL 1.1.x is not yet supported, but 1.0.2M has been
tested.

Use `-DENABLE_SSL=ON` when running CMake to build with OpenSSL.

Note that it will link to OpenSSL dynamically, so if the built executables are
deployed elsewhere, that machine also needs OpenSSL installed.

Beware that the OpenSSL installation, nor Mesos itself, comes with a certificate
bundle, and so it is likely that certificate verification will fail.
