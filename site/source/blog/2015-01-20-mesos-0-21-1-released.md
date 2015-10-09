---
layout: post
title: Apache Mesos 0.21.1 Released
permalink: /blog/mesos-0-21-1-released/
published: true
post_author:
  display_name: Tim Chen
  twitter: tnachen
tags: Release
---

The latest Mesos release, 0.21.1 is now available for [download](http://mesos.apache.org/downloads). This release includes bug fixes that fixes a few performance issues and isolator cleanup, and also includes a few improvements around building mesos on OSX and the docker containerizer.

* [MESOS-2047](https://issues.apache.org/jira/browse/MESOS-2047) - Isolator cleanup failures shouldn't cause TASK_LOST.
* [MESOS-2071](https://issues.apache.org/jira/browse/MESOS-2071) - Libprocess generates invalid HTTP
* [MESOS-2147](https://issues.apache.org/jira/browse/MESOS-2147) - Large number of connections slows statistics.json responses.
* [MESOS-2182](https://issues.apache.org/jira/browse/MESOS-2182) - Performance issue in libprocess SocketManager.
* [MESOS-1925](https://issues.apache.org/jira/browse/MESOS-1925) - Docker kill does not allow containers to exit gracefully
* [MESOS-2113](https://issues.apache.org/jira/browse/MESOS-2113) - Improve configure to find apr and svn libraries/headers in OSX

Full release notes are available in the release [CHANGELOG](https://github.com/apache/mesos/blob/master/CHANGELOG).

Upgrading to 0.21.1 can be done seamlessly on a 0.21.0 cluster. If upgrading from an earlier version, please refer to the [upgrades](http://mesos.apache.org/documentation/latest/upgrades/) documentation.

## Contributors
Thanks to all the contributors for 0.21.1: Timothy Chen, Benjamin Mahler, Kapil Arya, Ryan Thomas, Jie Yu, Dario Rexin and Michael Park.
