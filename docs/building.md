---
title: Apache Mesos - Building
layout: documentation
---

# Building

## Downloading Mesos

There are different ways you can get Mesos:

1\. Download the latest stable release from [Apache](http://mesos.apache.org/downloads/) (***Recommended***)

    $ wget https://downloads.apache.org/mesos/1.11.0/mesos-1.11.0.tar.gz
    $ tar -zxf mesos-1.11.0.tar.gz

2\. Clone the Mesos git [repository](https://gitbox.apache.org/repos/asf/mesos.git) (***Advanced Users Only***)

    $ git clone https://gitbox.apache.org/repos/asf/mesos.git

*NOTE: If you have problems running the above commands, you may need to first run through the ***System Requirements*** section below to install the `wget`, `tar`, and `git` utilities for your system.*

## System Requirements

Mesos runs on Linux (64 Bit) and Mac OS X (64 Bit). To build Mesos from source, GCC 4.8.1+ or Clang 3.5+ is required.

On Linux, a kernel version >= 2.6.28 is required at both build time and run time. For full support of process isolation under Linux a recent kernel >= 3.10 is required.

The Mesos agent also runs on Windows. To build Mesos from source, follow the instructions in the [Windows](windows.md) section.

Make sure your hostname is resolvable via DNS or via `/etc/hosts` to allow full support of Docker's host-networking capabilities, needed for some of the Mesos tests. When in doubt, please validate that `/etc/hosts` contains your hostname.

### Ubuntu 14.04

Following are the instructions for stock Ubuntu 14.04. If you are using a different OS, please install the packages accordingly.

    # Update the packages.
    $ sudo apt-get update

    # Install a few utility tools.
    $ sudo apt-get install -y tar wget git

    # Install the latest OpenJDK.
    $ sudo apt-get install -y openjdk-7-jdk

    # Install autotools (Only necessary if building from git repository).
    $ sudo apt-get install -y autoconf libtool

    # Install other Mesos dependencies.
    $ sudo apt-get -y install build-essential python-dev python-six python-virtualenv libcurl4-nss-dev libsasl2-dev libsasl2-modules maven libapr1-dev libsvn-dev

### Ubuntu 16.04

Following are the instructions for stock Ubuntu 16.04. If you are using a different OS, please install the packages accordingly.

    # Update the packages.
    $ sudo apt-get update

    # Install a few utility tools.
    $ sudo apt-get install -y tar wget git

    # Install the latest OpenJDK.
    $ sudo apt-get install -y openjdk-8-jdk

    # Install autotools (Only necessary if building from git repository).
    $ sudo apt-get install -y autoconf libtool

    # Install other Mesos dependencies.
    $ sudo apt-get -y install build-essential python-dev python-six python-virtualenv libcurl4-nss-dev libsasl2-dev libsasl2-modules maven libapr1-dev libsvn-dev zlib1g-dev iputils-ping

### Mac OS X 10.11 (El Capitan), macOS 10.12 (Sierra)

Following are the instructions for Mac OS X El Capitan. When building Mesos with the Apple-provided toolchain, the Command Line Tools from XCode >= 8.0 are required; XCode 8 requires Mac OS X 10.11.5 or newer.

    # Install Python 3: https://www.python.org/downloads/

    # Install Command Line Tools. The Command Line Tools from XCode >= 8.0 are required.
    $ xcode-select --install

    # Install Homebrew.
    $ ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"

    # Install Java.
    $ brew install Caskroom/cask/java

    # Install libraries.
    $ brew install wget git autoconf automake libtool subversion maven xz

    # Install Python dependencies.
    $ sudo easy_install pip
    $ pip install virtualenv

When compiling on macOS 10.12, the following is needed:

    # There is an incompatibility with the system installed svn and apr headers.
    # We need the svn and apr headers from a brew installation of subversion.
    # You may need to unlink the existing version of subversion installed via
    # brew in order to configure correctly.
    $ brew unlink subversion # (If already installed)
    $ brew install subversion

    # When configuring, the svn and apr headers from brew will be automatically
    # detected, so no need to explicitly point to them.
    # If the build fails due to compiler warnings, `--disable-werror` can be passed
    # to configure to not treat warnings as errors.
    $ ../configure

    # Lastly, you may encounter the following error when the libprocess tests run:
    $ ./libprocess-tests
    Failed to obtain the IP address for '<hostname>'; the DNS service may not be able to resolve it: nodename nor servname provided, or not known

    # If so, turn on 'Remote Login' within System Preferences > Sharing to resolve the issue.

*NOTE: When upgrading from Yosemite to El Capitan, make sure to rerun `xcode-select --install` after the upgrade.*

### CentOS 6.6

Following are the instructions for stock CentOS 6.6. If you are using a different OS, please install the packages accordingly.

    # Install a recent kernel for full support of process isolation.
    $ sudo rpm --import https://www.elrepo.org/RPM-GPG-KEY-elrepo.org
    $ sudo rpm -Uvh http://www.elrepo.org/elrepo-release-6-6.el6.elrepo.noarch.rpm
    $ sudo yum --enablerepo=elrepo-kernel install -y kernel-lt

    # Make the just installed kernel the one booted by default, and reboot.
    $ sudo sed -i 's/default=1/default=0/g' /boot/grub/grub.conf
    $ sudo reboot

    # Install a few utility tools. This also forces an update of `nss`,
    # which is necessary for the Java bindings to build properly.
    $ sudo yum install -y tar wget git which nss

    # 'Mesos > 0.21.0' requires a C++ compiler with full C++11 support,
    # (e.g. GCC > 4.8) which is available via 'devtoolset-2'.
    # Fetch the Scientific Linux CERN devtoolset repo file.
    $ sudo wget -O /etc/yum.repos.d/slc6-devtoolset.repo http://linuxsoft.cern.ch/cern/devtoolset/slc6-devtoolset.repo

    # Import the CERN GPG key.
    $ sudo rpm --import http://linuxsoft.cern.ch/cern/centos/7/os/x86_64/RPM-GPG-KEY-cern

    # Fetch the Apache Maven repo file.
    $ sudo wget http://repos.fedorapeople.org/repos/dchen/apache-maven/epel-apache-maven.repo -O /etc/yum.repos.d/epel-apache-maven.repo

    # 'Mesos > 0.21.0' requires 'subversion > 1.8' devel package, which is
    # not available in the default repositories.
    # Create a WANdisco SVN repo file to install the correct version:
    $ sudo bash -c 'cat > /etc/yum.repos.d/wandisco-svn.repo <<EOF
    [WANdiscoSVN]
    name=WANdisco SVN Repo 1.8
    enabled=1
    baseurl=http://opensource.wandisco.com/centos/6/svn-1.8/RPMS/$basearch/
    gpgcheck=1
    gpgkey=http://opensource.wandisco.com/RPM-GPG-KEY-WANdisco
    EOF'

    # Install essential development tools.
    $ sudo yum groupinstall -y "Development Tools"

    # Install 'devtoolset-2-toolchain' which includes GCC 4.8.2 and related packages.
    # Installing 'devtoolset-3' might be a better choice since `perf` might
    # conflict with the version of `elfutils` included in devtoolset-2.
    $ sudo yum install -y devtoolset-2-toolchain

    # Install other Mesos dependencies.
    $ sudo yum install -y apache-maven python-devel python-six python-virtualenv java-1.7.0-openjdk-devel zlib-devel libcurl-devel openssl-devel cyrus-sasl-devel cyrus-sasl-md5 apr-devel subversion-devel apr-util-devel

    # Enter a shell with 'devtoolset-2' enabled.
    $ scl enable devtoolset-2 bash
    $ g++ --version  # Make sure you've got GCC > 4.8!

    # Process isolation is using cgroups that are managed by 'cgconfig'.
    # The 'cgconfig' service is not started by default on CentOS 6.6.
    # Also the default configuration does not attach the 'perf_event' subsystem.
    # To do this, add 'perf_event = /cgroup/perf_event;' to the entries in '/etc/cgconfig.conf'.
    $ sudo yum install -y libcgroup
    $ sudo service cgconfig start

### CentOS 7.1

Following are the instructions for stock CentOS 7.1. If you are using a different OS, please install the packages accordingly.

    # Install a few utility tools
    $ sudo yum install -y tar wget git

    # Fetch the Apache Maven repo file.
    $ sudo wget http://repos.fedorapeople.org/repos/dchen/apache-maven/epel-apache-maven.repo -O /etc/yum.repos.d/epel-apache-maven.repo

    # Install the EPEL repo so that we can pull in 'libserf-1' as part of our
    # subversion install below.
    $ sudo yum install -y epel-release

    # 'Mesos > 0.21.0' requires 'subversion > 1.8' devel package,
    # which is not available in the default repositories.
    # Create a WANdisco SVN repo file to install the correct version:
    $ sudo bash -c 'cat > /etc/yum.repos.d/wandisco-svn.repo <<EOF
    [WANdiscoSVN]
    name=WANdisco SVN Repo 1.9
    enabled=1
    baseurl=http://opensource.wandisco.com/centos/7/svn-1.9/RPMS/\$basearch/
    gpgcheck=1
    gpgkey=http://opensource.wandisco.com/RPM-GPG-KEY-WANdisco
    EOF'

    # Parts of Mesos require systemd in order to operate. However, Mesos
    # only supports versions of systemd that contain the 'Delegate' flag.
    # This flag was first introduced in 'systemd version 218', which is
    # lower than the default version installed by centos. Luckily, centos
    # 7.1 has a patched 'systemd < 218' that contains the 'Delegate' flag.
    # Explicity update systemd to this patched version.
    $ sudo yum update systemd

    # Install essential development tools.
    $ sudo yum groupinstall -y "Development Tools"

    # Install other Mesos dependencies.
    $ sudo yum install -y apache-maven python-devel python-six python-virtualenv java-1.8.0-openjdk-devel zlib-devel libcurl-devel openssl-devel cyrus-sasl-devel cyrus-sasl-md5 apr-devel subversion-devel apr-util-devel

### Windows

Follow the instructions in the [Windows](windows.md) section.

## Building Mesos (Posix)

    # Change working directory.
    $ cd mesos

    # Bootstrap (Only required if building from git repository).
    $ ./bootstrap

    # Configure and build.
    $ mkdir build
    $ cd build
    $ ../configure
    $ make

In order to speed up the build and reduce verbosity of the logs, you can append `-j <number of cores> V=0` to `make`.

    # Run test suite.
    $ make check

    # Install (Optional).
    $ make install

## Examples

Mesos comes bundled with example frameworks written in C++, Java and Python.
The framework binaries will only be available after running `make check`, as
described in the ***Building Mesos*** section above.

    # Change into build directory.
    $ cd build

    # Start Mesos master (ensure work directory exists and has proper permissions).
    $ ./bin/mesos-master.sh --ip=127.0.0.1 --work_dir=/var/lib/mesos

    # Start Mesos agent (ensure work directory exists and has proper permissions).
    $ ./bin/mesos-agent.sh --master=127.0.0.1:5050 --work_dir=/var/lib/mesos

    # Visit the Mesos web page.
    $ http://127.0.0.1:5050

    # Run C++ framework (exits after successfully running some tasks).
    $ ./src/test-framework --master=127.0.0.1:5050

    # Run Java framework (exits after successfully running some tasks).
    $ ./src/examples/java/test-framework 127.0.0.1:5050

    # Run Python framework (exits after successfully running some tasks).
    $ ./src/examples/python/test-framework 127.0.0.1:5050

*Note: These examples assume you are running Mesos on your local machine.
Following them will not allow you to access the Mesos web page in a production
environment (e.g. on AWS). For that you will need to specify the actual IP of
your host when launching the Mesos master and ensure your firewall settings
allow access to port 5050 from the outside world.*
