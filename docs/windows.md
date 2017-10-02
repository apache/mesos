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

    # Start an administrative session of PowerShell
    # (required for creating symlinks when using the agent).

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
* The Mesos agent must be run as Administrator, mainly due to symlinks.
* The `MesosContainerizer` currently does not provide any actual
  resource isolation (similar to running the Mesos agent on POSIX).


## Status

For more information regarding the status of Windows support in Mesos,
please refer to the [JIRA epic](https://issues.apache.org/jira/browse/MESOS-3094).
