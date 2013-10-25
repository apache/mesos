# Getting Started #


### Downloading Mesos  ###

There are different ways you can get Mesos:

1. Download the latest stable release from [Apache](http://mesos.apache.org/downloads/) (***Recommended***)

        $ wget http://www.apache.org/dist/mesos/0.14.0/mesos-0.14.0.tar.gz
        $ tar -zxf mesos-0.14.0.tar.gz

2. Clone the Mesos git [repository](git-wip-us.apache.org/repos/asf/mesos.git) (***Advanced Users Only***)

        $ git clone http://git-wip-us.apache.org/repos/asf/mesos.git


### System Requirements ###

-  Mesos runs on Linux and Mac OSX.

-  Following are the instructions for stock Ubuntu 12.04 64 Bit. If you are using a different OS please install the packages accordingly.

        # Ensure apt-get is up to date.
        $ sudo apt-get update

        # Install build tools.
        $ sudo apt-get install build-essential

        # Install OpenJDK java.
        $ sudo apt-get install openjdk-6-jdk

        # Install devel python.
        $ sudo apt-get install python-dev

        # Install devel libcurl (***Optional***).
        $ sudo apt-get install libcurl4-nss-dev

        # Install devel libsasl (***Only required for Mesos 0.14.0 or newer***).
        $ sudo apt-get install libsasl2-dev


If you are building from git repository, you will need to additionally install the following packages.

        # Install autotoconf and automake.
        $ sudo apt-get install autoconf

        # Install libtool.
        $ sudo apt-get install libtool


***NOTES***

> 1. The build process attempts to guess where your Java include directory is, but if you have set the `$JAVA_HOME` environment variable, it will use `$JAVA_HOME/include`, which may not be correct (or exist) on your machine (in which case you will see an error such as: `configure: error: failed to build against JDK (using libtool)`). If this is the case, we suggest you unset the `JAVA_HOME` environment variable.

> 2. Mesos is currently being developed/tested/supported on 64 Bit machines only.


### Building Mesos ###

        # Change working directory.
        $ cd mesos

        # Bootstrap (***Skip this if you are not building from git repo***).
        $ ./bootstrap

        # Configure and build.
        $ mkdir build
        $ cd build
        $ ../configure
        $ make -j

        # Run test suite.
        $ make -j check

        # Install (***Optional***).
        $ make install



### Examples ###

Mesos comes bundled with example frameworks written in `C++`, `Java` and `Python`.

        # Change into build directory.
        $ cd build

        # Start mesos master.
        $ ./bin/mesos-master.sh --ip=127.0.0.1

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


> NOTE: To build the example frameworks, make sure you build the test suite by doing `make check`.
