---
layout: post
title: Apache Mesos 1.0.3 Released
permalink: /blog/mesos-1-0-3-released/
published: true
post_author:
  display_name: Vinod Kone
  twitter: vinodkone
tags: Release
---

The latest Mesos release, 1.0.3, is now available for [download](http://mesos.apache.org/downloads). This release includes some important bug fixes and improvements on top of 1.0.0. It is highly recommended to use this version if you are considering using Mesos 1.0. More specifically, this release includes the following fixes and improvements:

[MESOS-6052](https://issues.apache.org/jira/browse/MESOS-6052) - Unable to launch containers on CNI networks on CoreOS

[MESOS-6142](https://issues.apache.org/jira/browse/MESOS-6142) - Frameworks may RESERVE for an arbitrary role.

[MESOS-6621](https://issues.apache.org/jira/browse/MESOS-6621) - SSL downgrade path will CHECK-fail when using both temporary and persistent sockets

[MESOS-6676](https://issues.apache.org/jira/browse/MESOS-6676) - Always re-link with scheduler during re-registration.

[MESOS-6917](https://issues.apache.org/jira/browse/MESOS-6917) - Segfault when the executor sets an invalid UUID when sending a status update.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.0.3)

### Upgrades

Rolling upgrades from a Mesos 1.0.0 cluster to Mesos 1.0.3 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.0.3.

### Try it out

We encourage you to try out this release and let us know what you think. If you run into any issues, please let us know on the [user mailing ld Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 9 contributors who made 1.0.3 possible

Aaron Wood, Alexander Rukletsov, Avinash sridharan, Benjamin Bannier, Gastón Kleiman, Joseph Wu, Michael Park,Neil Conway, Vinod Kone
