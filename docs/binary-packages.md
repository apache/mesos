---
title: Apache Mesos - Getting started using Binaries.
layout: documentation
---

# Binary Packages

## Downloading the Mesos RPM

Download and install the latest stable RPM binary from the [Bintray Repository](https://bintray.com/apache/mesos/):

    $ cat > /tmp/bintray-mesos-el.repo <<EOF
    #bintray-mesos-el - packages by mesos from Bintray
    [bintray-mesos-el]
    name=bintray-mesos-el
    baseurl=https://dl.bintray.com/apache/mesos/el/7/x86_64
    gpgcheck=0
    repo_gpgcheck=0
    enabled=1
    EOF

    $ sudo mv /tmp/bintray-mesos-el.repo /etc/yum.repos.d/bintray-mesos-el.repo

    $ sudo yum update

    $ sudo yum install mesos

The above instructions show how to install the latest version of Mesos for RHEL 7.
Substitute `baseurl` the with the appropriate URL for your operating system.

## Start Mesos Master and Agent.

The RPM installation creates the directory `/var/lib/mesos` that can be used as a work directory.

Start the Mesos master with the following command:

    $ mesos-master --work_dir=/var/lib/mesos

On a different terminal, start the Mesos agent, and associate it with the Mesos master started above:

    $ mesos-agent --work_dir=/var/lib/mesos --master=127.0.0.1:5050

This is the simplest way to try out Mesos after downloading the RPM. For more complex and production
setup instructions refer to the [Administration](http://mesos.apache.org/documentation/latest/#administration) section of the docs.
