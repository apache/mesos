---
title: Apache Mesos - Getting started using Binaries.
layout: documentation
---

# Binary Packages

## Downloading the Mesos binaries

Download and install [the latest stable Mesos binaries](https://mesos.apache.org/downloads/).

## Start Mesos Master and Agent

The RPM installation creates the directory `/var/lib/mesos` that can be used as a work directory.

Start the Mesos master with the following command:

    $ mesos-master --work_dir=/var/lib/mesos

On a different terminal, start the Mesos agent, and associate it with the Mesos master started above:

    $ mesos-agent --work_dir=/var/lib/mesos --master=127.0.0.1:5050

This is the simplest way to try out Mesos after downloading the RPM. For more complex and production
setup instructions refer to the [Administration](http://mesos.apache.org/documentation/latest/#administration) section of the docs.
