---
title: Apache Mesos - Windows
layout: documentation
---

# Windows

Mesos 1.0.0 introduced experimental support for Windows.

## Building Mesos

### System Requirements

1. Install the latest [Visual Studio 2017](https://www.visualstudio.com/downloads/):
   The "Community" edition is sufficient (and free of charge).
   During installation, choose the "Desktop development with C++" workload.

2. Install [CMake 3.8.0](https://cmake.org/download/) or later.
   During installation, choose to "Add CMake to the system PATH for all users".

3. Install [GNU patch for Windows](http://gnuwin32.sourceforge.net/packages/patch.htm).

4. If building from source, install [Git](https://git-scm.com/download/win).

5. Make sure there are no spaces in your build directory.
   For example, `C:/Program Files (x86)/mesos` is an invalid build directory.

6. If developing Mesos, install [Python 3](https://www.python.org/downloads/)
   (not Python 2), in order to use our `support` scripts (e.g.
   to post and apply patches, or lint source code).

### Build Instructions

Following are the instructions for Windows 10.

    # Clone (or extract) Mesos.
    git clone https://gitbox.apache.org/repos/asf/mesos.git
    cd mesos

    # Configure using CMake for an out-of-tree build.
    mkdir build
    cd build
    cmake .. -G "Visual Studio 15 2017 Win64" -T "host=x64"

    # Build Mesos.
    # To build just the Mesos agent, add `--target mesos-agent`.
    cmake --build .

    # The Windows agent exposes new isolators that must be used as with
    # the `--isolation` flag. To get started point the agent to a working
    # master, using eiher an IP address or zookeeper information.
    .\src\mesos-agent.exe --master=<master> --work_dir=<work folder> --launcher_dir=<repository>\build\src

## Running Mesos

If you deploy the executables to another machine, you must also
install the [Microsoft Visual C++ Redistributable for Visual Studio 2017](https://aka.ms/vs/15/release/VC_redist.x64.exe).

## Known Limitations

The current implementation is known to have the following limitations:

* Only the agent should be run on Windows. The Mesos master can be
  launched, but only for testing as the master does not support
  high-availability setups on Windows.

* While Mesos supports NTFS long paths internally, tasks which do not support
  long paths must be run on agent whose `--work_dir` is a short path.

* The minimum versions of Windows supported are: Windows 10 Creators Update (AKA
  version 1703, build number 15063), and [Windows Server, version 1709][server].
  It is likely that this will increase, due to evolving Windows container
  support and developer features which ease porting.

* The ability to [create symlinks][] as a non-admin user requires
  Developer Mode to be enabled. Otherwise the agent will need to be
  run under an administrator.

[server]: https://docs.microsoft.com/en-us/windows-server/get-started/get-started-with-1709
[create symlinks]: https://blogs.windows.com/buildingapps/2016/12/02/symlinks-windows-10/

## Build Configuration Examples

### Building with Ninja

Instead of using MSBuild, it is also possible to build Mesos on
Windows using [Ninja](https://ninja-build.org/), which can result in
significantly faster builds. To use Ninja, you need to download it and
ensure `ninja.exe` is in your `PATH`.

* Download the [Windows binary](https://github.com/ninja-build/ninja/releases).
* Unzip it and place `ninja.exe` in your `PATH`.
* Open an "x64 Native Tools Command Prompt for VS 2017" to set your
  environment.
* In that command prompt, type `powershell` to use a better shell.
* Similar to above, configure CMake with `cmake .. -G Ninja`.
* Now you can use `ninja` to build the various targets.
* You may want to use `ninja -v` to make it verbose, as it's otherwise
  very quiet.

Note that with Ninja it is imperative to open the correct developer
command prompt so that the 64-bit build tools are used, as Ninja does
not otherwise know how to find them.

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
cmake .. -DENABLE_JAVA=ON -G "Visual Studio 15 2017 Win64" -T "host=x64"
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

As of this writing, OpenSSL 1.1.x is supported.

Use `-DENABLE_SSL=ON` to build with OpenSSL.

Note that it will link to OpenSSL dynamically, so if the built executables are
deployed elsewhere, that machine also needs OpenSSL installed.

Beware that the OpenSSL installation, nor Mesos itself, comes with a certificate
bundle, and so it is likely that certificate verification will fail.
