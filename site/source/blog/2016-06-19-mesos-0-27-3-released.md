---
layout: post
title: Apache Mesos 0.27.3 Released
permalink: /blog/mesos-0-27-3-released/
published: true
post_author:
  display_name: Jie Yu
  twitter: jie_yu
tags: Release
---

The latest Mesos point release for 0.27, 0.27.3, is now available for
[download](http://mesos.apache.org/downloads). This release includes fixes and
improvements for the following:

* [MESOS-4705] - Linux 'perf' parsing logic may fail when OS distribution has perf backports.
* [MESOS-4869] - /usr/libexec/mesos/mesos-health-check using/leaking a lot of memory.
* [MESOS-5018] - FrameworkInfo Capability enum does not support upgrades.
* [MESOS-5021] - Memory leak in subprocess when 'environment' argument is provided.
* [MESOS-5449] - Memory leak in SchedulerProcess.declineOffer.

Full release notes are available in the release
[CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=0.27.3).

### Upgrades

Rolling upgrades from a Mesos 0.27.2 cluster to Mesos 0.27.3 are
straightforward. Please refer to the [upgrade
guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed
information on upgrading to Mesos 0.27.3.

### Try it out

We encourage you to try out this release and let us know what you think.  If you
run into any issues, please let us know on the [user mailing list and
IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 4 contributors who made 0.27.3 possible:

Fan Du, Vinod Kone, Benjamin Mahler, Dario Rexin, Yongqiao Wang, Jie Yu
