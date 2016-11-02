---
title: Apache Mesos - Windows
layout: documentation
---

# Windows

Mesos 1.0.0 introduced experimental support for Windows.


## Building Mesos


### System Requirements

1. Install the latest version of [Visual Studio Community 2015](https://www.visualstudio.com/post-download-vs?sku=community).
   Make sure to select the Common Tools for Visual C++ and the Windows 10 SDK.
   Start Visual Studio Community to complete the setup and configuration.
2. Install [CMake 3.5.2 or later](https://cmake.org/files/v3.5/cmake-3.5.2-win32-x86.msi).
   Do not run CMake before finishing the Visual Studio Community setup.
3. Install [Gnu Patch 2.5.9-7 or later](http://downloads.sourceforge.net/project/gnuwin32/patch/2.5.9-7/patch-2.5.9-7-setup.exe).
4. If building from git, make sure you have Windows-style line endings.
   i.e. `git config core.autocrlf true`.
5. Make sure there are no spaces in your build directory.
   For example, `C:/Program Files (x86)/mesos` is an invalid build directory.


### Build Instructions

Following are the instructions for stock Windows 10 and Windows Server 2012 or newer.

    # Start a VS2015 x64 Native Tool command prompt.
    # This can be found by opening VS2015 and looking under the "tools"
    # menu for "Visual Studio Command Prompt".

    # Change working directory.
    $ cd mesos

    # If you are developing on Windows, we recommend running the bootstrap.
    # This requires administrator privileges.
    $ .\bootstrap.bat

    # Generate the solution and build.
    $ mkdir build
    $ cd build
    $ cmake .. -G "Visual Studio 14 2015 Win64" -DENABLE_LIBEVENT=1

    # After generating the Visual Studio solution you can use the IDE to open
    # the project and skip the next step. In this case it is recommended to set
    # `PreferredToolArchitecture` environment variable to `x64`.
    # NOTE: `PreferredToolArchitecture` can be set system-wide via Control Panel.
    $ msbuild Mesos.sln /p:PreferredToolArchitecture=x64

    # mesos-agent.exe can be found in the <repository>\build\src folder.
    $ cd src

    # The Windows agent exposes new isolators that must be used as with
    # the `--isolation` flag. To get started point the agent to a working
    # master, using eiher an IP address or zookeeper information.
    $ mesos-agent.exe --master=<master> --work_dir=<work folder> --isolation=windows/cpu,filesystem/windows --launcher_dir=<repository>\build\src


## Known Limitations

The current implementation is known to have the following limitations:

* At this point, only the agent is capable of running on Windows,
  the Mesos master must run on a Posix machine.
* Due to the 260 character `MAX_PATH` limitation on Windows,
  it is required to set the configuration option `--launcher_dir`
  to be a root path, e.g. `C:\`.
  In addition, the `TASK_ID` that is passed to Mesos should be short,
  up to about 40 characters. **NOTE**: If you schedule tasks via Marathon,
  your Marathon task id should be up to four characters long since Marathon
  generates Mesos `TASK_ID` by appending a UUID (36 characters) onto
  the Marathon task id.
* Currently runs as Administrator (mainly due to symlinks).
* Only the `MesosContainerizer` is currently supported,
  which does not provide resource isolation on Windows.
  Resource isolation will be provided via the `DockerContainerizer`
  (e.g. Windows Containers) in the future.
* Most of the tests are not ported to Windows.


## Status

For more information regarding the status of Windows support in Mesos,
please refer to the [Jira epic](https://issues.apache.org/jira/browse/MESOS-3094).
