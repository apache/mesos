---
layout: post
title: Apache Mesos 1.1.1 Released
permalink: /blog/mesos-1-1-1-released/
published: true
post_author:
  display_name: AlexR
tags: Release
---

The latest Mesos release, 1.1.1, is now available for
[download](http://mesos.apache.org/downloads). This release includes some
important bug fixes and improvements on top of 1.1.0. It is highly recommended
that users use this version if you are considering using Mesos 1.1. More
specifically, this release includes the following major fixes and improvements:

* [MESOS-6002](https://issues.apache.org/jira/browse/MESOS-6002) - The whiteout file cannot be removed correctly using aufs backend.
* [MESOS-6010](https://issues.apache.org/jira/browse/MESOS-6010) - Docker registry puller shows decode error "No response decoded".
* [MESOS-6142](https://issues.apache.org/jira/browse/MESOS-6142) - Frameworks may RESERVE for an arbitrary role.
* [MESOS-6360](https://issues.apache.org/jira/browse/MESOS-6360) - The handling of whiteout files in provisioner is not correct.
* [MESOS-6411](https://issues.apache.org/jira/browse/MESOS-6411) - Add documentation for CNI port-mapper plugin.
* [MESOS-6526](https://issues.apache.org/jira/browse/MESOS-6526) - `mesos-containerizer launch --environment` exposes executor env vars in `ps`.
* [MESOS-6571](https://issues.apache.org/jira/browse/MESOS-6571) - Add "--task" flag to mesos-execute
* [MESOS-6597](https://issues.apache.org/jira/browse/MESOS-6597) - Include v1 Operator API protos in generated JAR and python packages.
* [MESOS-6606](https://issues.apache.org/jira/browse/MESOS-6606) - Reject optimized builds with libcxx before 3.9
* [MESOS-6621](https://issues.apache.org/jira/browse/MESOS-6621) - SSL downgrade path will CHECK-fail when using both temporary and persistent sockets
* [MESOS-6624](https://issues.apache.org/jira/browse/MESOS-6624) - Master WebUI does not work on Firefox 45
* [MESOS-6676](https://issues.apache.org/jira/browse/MESOS-6676) - Always re-link with scheduler during re-registration.
* [MESOS-6848](https://issues.apache.org/jira/browse/MESOS-6848) - The default executor does not exit if a single task pod fails.
* [MESOS-6852](https://issues.apache.org/jira/browse/MESOS-6852) - Nested container's launch command is not set correctly in docker/runtime isolator.
* [MESOS-6917](https://issues.apache.org/jira/browse/MESOS-6917) - Segfault when the executor sets an invalid UUID when sending a status update.
* [MESOS-7008](https://issues.apache.org/jira/browse/MESOS-7008) - Quota not recovered from registry in empty cluster.
* [MESOS-7133](https://issues.apache.org/jira/browse/MESOS-7133) - mesos-fetcher fails with openssl-related output.

Full release notes are available in the release
[CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.1.1)

### Upgrades

Rolling upgrades from a Mesos 1.1.0 cluster to Mesos 1.1.1 are straightforward.
Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/)
for detailed information on upgrading to Mesos 1.1.1.

### Try it out

We encourage you to try out this release and let us know what you think. If you
run into any issues, please let us know on the
[user mailing list and IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 15 contributors who made 1.1.1 possible:

Aaron Wood, Alexander Rukletsov, Anand Mazumdar, Avinash sridharan, Benjamin Bannier, Gastón Kleiman, Gilbert Song, Haosdent Huang, Jan Schlicht, Jiang Yan Xu, Jie Yu, Joseph Wu, Michael Park, Neil Conway, Qian Zhang, Till Toenshoff, Vijay Srinivasaraghavan, Vinod Kone, haosdent huang.
