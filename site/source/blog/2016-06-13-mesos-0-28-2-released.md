---
layout: post
title: Apache Mesos 0.28.2 Released
permalink: /blog/mesos-0-28-2-released/
published: true
post_author:
  display_name: Jie Yu
  twitter: jie_yu
tags: Release
---

The latest Mesos release, 0.28.2, is now available for [download](http://mesos.apache.org/downloads). This release includes some important bug fixes and improvements on top of 0.28.1. It is highly recommended that users use this version if you are considering using Mesos 0.28. More specifically, this release includes the following fixes and improvements:

* [MESOS-4705](https://issues.apache.org/jira/browse/MESOS-4705) - Linux 'perf' parsing logic may fail when OS distribution has perf backports.
* [MESOS-5239](https://issues.apache.org/jira/browse/MESOS-5239) - Persistent volume DockerContainerizer support assumes proper mount propagation setup on the host.
* [MESOS-5253](https://issues.apache.org/jira/browse/MESOS-5253) - Isolator cleanup should not be invoked if they are not prepared yet.
* [MESOS-5282](https://issues.apache.org/jira/browse/MESOS-5282) - Destroy container while provisioning volume images may lead to a race.
* [MESOS-5312](https://issues.apache.org/jira/browse/MESOS-5312) - Env `MESOS_SANDBOX` is not set properly for command tasks that changes rootfs.
* [MESOS-4885](https://issues.apache.org/jira/browse/MESOS-4885) - Unzip should force overwrite.
* [MESOS-5449](https://issues.apache.org/jira/browse/MESOS-5449) - Memory leak in SchedulerProcess.declineOffer.
* [MESOS-5380](https://issues.apache.org/jira/browse/MESOS-5380) - Killing a queued task can cause the corresponding command executor to never terminate.
* [MESOS-5307](https://issues.apache.org/jira/browse/MESOS-5307) - Sandbox mounts should not be in the host mount namespace.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=0.28.2)

### Upgrades

Rolling upgrades from a Mesos 0.28.1 cluster to Mesos 0.28.2 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 0.28.2.

### Try it out

We encourage you to try out this release and let us know what you think. If you run into any issues, please let us know on the [user mailing list and IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 10 contributors who made 0.28.2 possible:

Fan Du, Tomasz Janiszewski, Vinod Kone, Shuai Lin, Ben Mahler, Dario Rexin, Silas Snider, Gilbert Song, Yongqiao Wang, Jie Yu
