---
layout: post
title: Apache Mesos 0.28.3 Released
permalink: /blog/mesos-0-28-3-released/
published: true
post_author:
  display_name: Anand Mazumdar
tags: Release
---

The latest Mesos release, 0.28.3, is now available for [download](http://mesos.apache.org/downloads). This release includes some important bug fixes and improvements on top of 0.28.2. It is highly recommended that users use this version if you are considering using Mesos 0.28. More specifically, this release includes the following major fixes and improvements:

* [MESOS-2043](https://issues.apache.org/jira/browse/MESOS-2043) - Framework auth fail with timeout error and never get authenticated
* [MESOS-5571](https://issues.apache.org/jira/browse/MESOS-5571) - Scheduler JNI throws exception when the major versions of JAR and libmesos don't match.
* [MESOS-5691](https://issues.apache.org/jira/browse/MESOS-5691) - SSL downgrade support will leak sockets in CLOSE_WAIT status.
* [MESOS-5698](https://issues.apache.org/jira/browse/MESOS-5698) - Quota sorter not updated for resource changes at agent.
* [MESOS-5723](https://issues.apache.org/jira/browse/MESOS-5723) - SSL-enabled libprocess will leak incoming links to forks.
* [MESOS-5763](https://issues.apache.org/jira/browse/MESOS-5763) - Task stuck in fetching is not cleaned up after --executor_registration_timeout.
* [MESOS-5913](https://issues.apache.org/jira/browse/MESOS-5913) - Stale socket FD usage when using libevent + SSL.
* [MESOS-5943](https://issues.apache.org/jira/browse/MESOS-5943) - Incremental http parsing of URLs leads to decoder error.
* [MESOS-5986](https://issues.apache.org/jira/browse/MESOS-5986) - SSL Socket CHECK can fail after socket receives EOF.
* [MESOS-6104](https://issues.apache.org/jira/browse/MESOS-6104) - Potential FD double close in libevent's
* [MESOS-6142](https://issues.apache.org/jira/browse/MESOS-6142) - Frameworks may RESERVE for an arbitrary role.
* [MESOS-6233](https://issues.apache.org/jira/browse/MESOS-6233) - Master CHECK fails during recovery while relinking to other masters.
* [MESOS-6299](https://issues.apache.org/jira/browse/MESOS-6299) - Master doesn't remove task from pending when it is invalid.
* [MESOS-6457](https://issues.apache.org/jira/browse/MESOS-6457) - Tasks shouldn't transition from TASK_KILLING to TASK_RUNNING.
* [MESOS-6502](https://issues.apache.org/jira/browse/MESOS-6502) - \_version uses incorrect MESOS_{MAJOR,MINOR,PATCH}_VERSION in libmesos java binding.
* [MESOS-6527](https://issues.apache.org/jira/browse/MESOS-6527) - Memory leak in the libprocess request decoder.
* [MESOS-6621](https://issues.apache.org/jira/browse/MESOS-6621) - SSL downgrade path will CHECK-fail when using both temporary and persistent sockets.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=0.28.3)

### Upgrades

Rolling upgrades from a Mesos 0.28.2 cluster to Mesos 0.28.3 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 0.28.3.

### Try it out

We encourage you to try out this release and let us know what you think. If you run into any issues, please let us know on the [user mailing list and IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 15 contributors who made 0.28.3 possible:

Anand Mazumdar, Benjamin Bannier, Benjamin Mahler, David Robinson, Gastón Kleiman, Gilbert Song, Greg Mann, Joris Van Remoortere, Joseph Wu, Neil Conway, Silas Snider, Vinod Kone, Yan Xu, Zhitao Li, Zhou Xing
