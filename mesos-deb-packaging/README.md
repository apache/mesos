# Mesos Debian packaging

Build scripts to create a Mesos Debian package with [FPM](https://github.com/jordansissel/fpm) for simple installation in a cluster. 

Mesos is a cluster manager that provides efficient resource isolation and sharing across distributed applications, or frameworks. It can run Hadoop, MPI, Hypertable, Spark (a new framework for low-latency interactive and iterative jobs), and other applications. Currently is in the Apache Incubator and going through rapid development, though stable enough for a production usage. See [Mesos website](http://incubator.apache.org/mesos/) for more details.

## Requirements

  * ruby
  * prerequisites:

       gem install fpm
       sudo apt-get install python-dev autoconf automake git make libssl-dev libcurl3

define in e.g. `~/.bash_profile` a `MAINTAINER` variable

	export MAINTAINER="email@example.com"

## Building deb package

	./build_mesos wheezy

## TODO

   * automatic restart of master/slave when upgrading
   * logrotate support
   * service autostart

## Authors

   * Tomas Barton

