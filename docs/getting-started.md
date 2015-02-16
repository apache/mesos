---
layout: documentation
---

# Getting Started

## Downloading Mesos

There are different ways you can get Mesos:

1. Download the latest stable release from [Apache](http://mesos.apache.org/downloads/) (***Recommended***)

        $ wget http://www.apache.org/dist/mesos/0.20.1/mesos-0.20.1.tar.gz
        $ tar -zxf mesos-0.20.1.tar.gz

2. Clone the Mesos git [repository](https://git-wip-us.apache.org/repos/asf/mesos.git) (***Advanced Users Only***)

        $ git clone https://git-wip-us.apache.org/repos/asf/mesos.git

## System Requirements

-  Mesos runs on Linux (64 Bit) and Mac OSX (64 Bit).

### Ubuntu 12.04

-  Following are the instructions for stock Ubuntu 12.04 64 Bit. If you are using a different OS please install the packages accordingly.

        # Ensure apt-get is up to date.
        $ sudo apt-get update

        # Install build tools.
        $ sudo apt-get install build-essential

        # Install OpenJDK java.
        $ sudo apt-get install openjdk-6-jdk

        # Install devel python.
        $ sudo apt-get install python-dev python-boto

        # Install devel libcurl
        $ sudo apt-get install libcurl4-nss-dev

        # Install devel libsasl (***Only required for Mesos 0.14.0 or newer***).
        $ sudo apt-get install libsasl2-dev

        # Install Maven (***Only required for Mesos 0.18.1 or newer***).
        $ sudo apt-get install maven

        # Install devel libapr1 (***Only required for Mesos 0.21.0 or newer***)
        $ sudo apt-get install libapr1-dev

        # Install devel libsvn (***Only required for Mesos 0.21.0 or newer***)
        $ sudo apt-get install libsvn-dev

-  If you are building from git repository, you will need to additionally install the following packages.

        # Install autotoconf and automake.
        $ sudo apt-get install autoconf

        # Install libtool.
        $ sudo apt-get install libtool

### CentOS 6.5

- Following are the instructions for stock CentOS 6.5. If you are using a different OS, please install the packages accordingly.

        Mesos 0.21.0+ requires subversion 1.8+ devel package which is not available by default by yum.
        Add one of the repo that has subversion-devel 1.8 available, i.e:

        Add new repo /etc/yum.repos.d/wandisco-svn.repo, with:

        [WandiscoSVN]
        name=Wandisco SVN Repo
        baseurl=http://opensource.wandisco.com/centos/6/svn-1.8/RPMS/$basearch/
        enabled=1
        gpgcheck=0

        $ sudo yum groupinstall -y "Development Tools"

        $ sudo yum install -y python-devel java-1.7.0-openjdk-devel zlib-devel libcurl-devel openssl-devel cyrus-sasl-devel cyrus-sasl-md5 apr-devel subversion-devel apr-utils-devel

        # Install maven.
        $ wget http://mirror.nexcess.net/apache/maven/maven-3/3.0.5/binaries/apache-maven-3.0.5-bin.tar.gz
        $ sudo tar -zxf apache-maven-3.0.5-bin.tar.gz -C /opt/
        $ sudo ln -s /opt/apache-maven-3.0.5/bin/mvn /usr/bin/mvn

## Building Mesos

        # Change working directory.
        $ cd mesos

        # Bootstrap (***Skip this if you are not building from git repo***).
        $ ./bootstrap

        # Configure and build.
        $ mkdir build
        $ cd build
        $ ../configure
        $ make

        # Run test suite.
        $ make check

        # Install (***Optional***).
        $ make install

## Examples
-  Mesos comes bundled with example frameworks written in `C++`, `Java` and `Python`.

        # Change into build directory.
        $ cd build

        # Start mesos master (***Ensure work directory exists and has proper permissions***).
        $ ./bin/mesos-master.sh --ip=127.0.0.1 --work_dir=/var/lib/mesos

        # Start mesos slave.
        $ ./bin/mesos-slave.sh --master=127.0.0.1:5050

        # Visit the mesos web page.
        $ http://127.0.0.1:5050

        # Run C++ framework (***Exits after successfully running some tasks.***).
        $ ./src/test-framework --master=127.0.0.1:5050

        # Run Java framework (***Exits after successfully running some tasks.***).
        $ ./src/examples/java/test-framework 127.0.0.1:5050

        # Run Python framework (***Exits after successfully running some tasks.***).
        $ ./src/examples/python/test-framework 127.0.0.1:5050

*NOTE: To build the example frameworks, make sure you build the test suite by doing `make check`.*
