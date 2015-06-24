---
layout: documentation
---

# Getting Started

## Downloading Mesos

There are different ways you can get Mesos:

1. Download the latest stable release from [Apache](http://mesos.apache.org/downloads/) (***Recommended***)

        $ wget http://www.apache.org/dist/mesos/0.22.1/mesos-0.22.1.tar.gz
        $ tar -zxf mesos-0.22.1.tar.gz

2. Clone the Mesos git [repository](https://git-wip-us.apache.org/repos/asf/mesos.git) (***Advanced Users Only***)

        $ git clone https://git-wip-us.apache.org/repos/asf/mesos.git

## System Requirements

Mesos runs on Linux (64 Bit) and Mac OS X (64 Bit).

### Ubuntu 14.04

Following are the instructions for stock Ubuntu 14.04. If you are using a different OS, please install the packages accordingly.

        # Update the packages.
        $ sudo apt-get update

        # Install the latest OpenJDK.
        $ sudo apt-get install -y openjdk-7-jdk

        # Install autotools (Only necessary if building from git repository).
        $ sudo apt-get install -y autoconf libtool

        # Install other Mesos dependencies.
        $ sudo apt-get -y install build-essential python-dev python-boto libcurl4-nss-dev libsasl2-dev maven libapr1-dev libsvn-dev

### Mac OS X Yosemite

Following are the instructions for stock Mac OS X Yosemite. If you are using a different OS, please install the packages accordingly.

        # Install Command Line Tools.
        $ xcode-select --install

        # Install Homebrew.
        $ ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"

        # Install libraries.
        $ brew install autoconf automake libtool subversion maven

### CentOS 6.6

Following are the instructions for stock CentOS 6.6. If you are using a different OS, please install the packages accordingly.

        # Install a few utility tools
        $ sudo yum install -y tar wget which

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
        # Add the WANdisco SVN repo file: '/etc/yum.repos.d/wandisco-svn.repo' with content:

          [WANdiscoSVN]
          name=WANdisco SVN Repo 1.8
          enabled=1
          baseurl=http://opensource.wandisco.com/centos/6/svn-1.8/RPMS/$basearch/
          gpgcheck=1
          gpgkey=http://opensource.wandisco.com/RPM-GPG-KEY-WANdisco

        # Install essential development tools.
        $ sudo yum groupinstall -y "Development Tools"

        # Install 'devtoolset-2-toolchain' which includes GCC 4.8.2 and related packages.
        $ sudo yum install -y devtoolset-2-toolchain

        # Install other Mesos dependencies.
        $ sudo yum install -y apache-maven python-devel java-1.7.0-openjdk-devel zlib-devel libcurl-devel openssl-devel cyrus-sasl-devel cyrus-sasl-md5 apr-devel subversion-devel apr-util-devel

        # Enter a shell with 'devtoolset-2' enabled.
        $ scl enable devtoolset-2 bash
        $ g++ --version  # Make sure you've got GCC > 4.8!

### CentOS 7.1

Following are the instructions for stock CentOS 7.1. If you are using a different OS, please install the packages accordingly.

        # Install a few utility tools
        $ sudo yum install -y tar wget

        # Fetch the Apache Maven repo file.
        $ sudo wget http://repos.fedorapeople.org/repos/dchen/apache-maven/epel-apache-maven.repo -O /etc/yum.repos.d/epel-apache-maven.repo

        # 'Mesos > 0.21.0' requires 'subversion > 1.8' devel package, which is
        # not available in the default repositories.
        # Add the WANdisco SVN repo file: '/etc/yum.repos.d/wandisco-svn.repo' with content:

          [WANdiscoSVN]
          name=WANdisco SVN Repo 1.9
          enabled=1
          baseurl=http://opensource.wandisco.com/centos/7/svn-1.9/RPMS/$basearch/
          gpgcheck=1
          gpgkey=http://opensource.wandisco.com/RPM-GPG-KEY-WANdisco

        # Install essential development tools.
        $ sudo yum groupinstall -y "Development Tools"

        # Install other Mesos dependencies.
        $ sudo yum install -y apache-maven python-devel java-1.7.0-openjdk-devel zlib-devel libcurl-devel openssl-devel cyrus-sasl-devel cyrus-sasl-md5 apr-devel subversion-devel apr-util-devel

## Building Mesos

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

        # Change into build directory.
        $ cd build

        # Start mesos master (Ensure work directory exists and has proper permissions).
        $ ./bin/mesos-master.sh --ip=127.0.0.1 --work_dir=/var/lib/mesos

        # Start mesos slave.
        $ ./bin/mesos-slave.sh --master=127.0.0.1:5050

        # Visit the mesos web page.
        $ http://127.0.0.1:5050

        # Run C++ framework (Exits after successfully running some tasks.).
        $ ./src/test-framework --master=127.0.0.1:5050

        # Run Java framework (Exits after successfully running some tasks.).
        $ ./src/examples/java/test-framework 127.0.0.1:5050

        # Run Python framework (Exits after successfully running some tasks.).
        $ ./src/examples/python/test-framework 127.0.0.1:5050

*NOTE: To build the example frameworks, make sure you build the test suite by doing `make check`.*
