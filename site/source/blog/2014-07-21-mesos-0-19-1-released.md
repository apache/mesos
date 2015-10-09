---
layout: post
title: Apache Mesos 0.19.1 Released
permalink: /blog/mesos-0-19-1-released/
published: true
post_author:
  display_name: Ben Mahler
  twitter: bmahler
tags: Release
---

The latest Mesos release, 0.19.1 is now available for [download](http://mesos.apache.org/downloads). This release fixes a bug in the Java and Python APIs in which the garbage collection of a MesosSchedulerDriver object leads to a framework unregistration.

More details can be found in [MESOS-1550](https://issues.apache.org/jira/browse/MESOS-1550), the [report thread](http://mail-archives.apache.org/mod_mbox/mesos-user/201406.mbox/%3CCAFeOQnXVBBfgg4WQFatkHix2B1y-1o3EH3-mqtsgbEDeMH4Tbw%40mail.gmail.com%3E) from Whitney Sorenson, and the [follow up thread](http://mail-archives.apache.org/mod_mbox/mesos-user/201406.mbox/%3CCAFeOQnVfhNo-mgkFsRn%3DvFsCQG%2Bj47xE3kdbFUDspWeOKjXA%3Dg%40mail.gmail.com%3E) from Benjamin Hindman.

This release has the following bug fixes:

 * [MESOS-1448](https://issues.apache.org/jira/browse/MESOS-1448) - Mesos Fetcher doesn't support URLs that have 30X redirects.
 * [MESOS-1534](https://issues.apache.org/jira/browse/MESOS-1534) - Scheduler process is not explicitly terminated in the destructor of MesosSchedulerDriver.
 * [MESOS-1538](https://issues.apache.org/jira/browse/MESOS-1538) - A container destruction in the middle of a launch leads to CHECK failure.
 * [MESOS-1539](https://issues.apache.org/jira/browse/MESOS-1539) - No longer able to spin up Mesos master in local mode.
 * [MESOS-1550](https://issues.apache.org/jira/browse/MESOS-1550) - MesosSchedulerDriver should never, ever, call 'stop'.
 * [MESOS-1551](https://issues.apache.org/jira/browse/MESOS-1551) - Master does not create work directory when missing.

Upgrading to 0.19.1 can be done seamlessly on a 0.19.0 cluster. If upgrading from 0.18.x, please refer to the [upgrades](http://mesos.apache.org/documentation/latest/upgrades/) documentation.

## Contributors
Thanks to all the contributors for 0.19.1: Tom Arnfeld, Ian Downes, Benjamin Hindman, Benjamin Mahler, Vinod Kone, Whitney Sorenson.