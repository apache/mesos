---
title: Apache Mesos - CMake
layout: documentation
---

# CMake

## Downloading Mesos

There are different ways you can get Mesos:

1\. Download the latest stable release from [Apache](http://mesos.apache.org/downloads/) (***Recommended***)

    $ wget http://www.apache.org/dist/mesos/0.28.1/mesos-0.28.1.tar.gz
    $ tar -zxf mesos-0.28.1.tar.gz

2\. Clone the Mesos git [repository](https://git-wip-us.apache.org/repos/asf/mesos.git) (***Advanced Users Only***)

    $ git clone https://git-wip-us.apache.org/repos/asf/mesos.git

*NOTE: If you have problems running the above commands, you may need to first run through the ***System Requirements*** section below to install the `wget`, `tar`, and `git` utilities for your system.*

## System Requirements

Mesos runs on Linux (64 Bit) and Mac OS X (64 Bit). To build Mesos from source, GCC 4.8.1+ or Clang 3.5+ and CMake 3.5.1 is required.

For full support of process isolation under Linux a recent kernel >=3.10 is required.

Make sure your hostname is resolvable via DNS or via `/etc/hosts` to allow full support of Docker's host-networking capabilities, needed for some of the Mesos tests. When in doubt, please validate that `/etc/hosts` contains your hostname.

### Ubuntu 16.04

Following are the instructions for stock Ubuntu 16.04. If you are using a different OS, please install the packages accordingly.

    # Update the packages.
    $ sudo apt-get update

    # Install a few utility tools.
    $ sudo apt-get install -y tar wget git

    # Install the latest OpenJDK.
    $ sudo apt-get install -y openjdk-7-jdk

    # Install autotools (Only necessary if building from git repository).
    $ sudo apt-get install -y autoconf libtool

    # Install other Mesos dependencies.
    $ sudo apt-get -y install build-essential python-dev libcurl4-nss-dev libsasl2-dev libsasl2-modules maven libapr1-dev libsvn-dev

## Building Mesos with CMake

    # Change working directory.
    $ cd mesos

    # Configure and build.
    $ mkdir build
    $ cd build
    $ cmake ..
    $ make
