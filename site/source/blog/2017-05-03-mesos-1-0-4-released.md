---
layout: post
title: Apache Mesos 1.0.4 Released
permalink: /blog/mesos-1-0-4-released/
published: true
post_author:
  display_name: Vinod Kone
  twitter: vinodkone
tags: Release
---

The latest Mesos release, 1.0.4, is now available for [download](http://mesos.apache.org/downloads). This release includes some important bug fixes and improvements on top of 1.0.0. It is highly recommended to use this version if you are considering using Mesos 1.0. More specifically, this release includes the following fixes and improvements:

[MESOS-2537](https://issues.apache.org/jira/browse/MESOS-2537) - AC_ARG_ENABLED checks are broken

[MESOS-6606](https://issues.apache.org/jira/browse/MESOS-6606) - Reject optimized builds with libcxx before 3.9

[MESOS-7008](https://issues.apache.org/jira/browse/MESOS-7008) - Quota not recovered from registry in empty cluster.

[MESOS-7265](https://issues.apache.org/jira/browse/MESOS-7265) - Containerizer startup may cause sensitive data to leak into sandbox logs.

[MESOS-7366](https://issues.apache.org/jira/browse/MESOS-7366) - Agent sandbox gc could accidentally delete the entire persistent volume content.

[MESOS-7383](https://issues.apache.org/jira/browse/MESOS-7383) - Docker executor logs possibly sensitive parameters.

[MESOS-7422](https://issues.apache.org/jira/browse/MESOS-7422) - Docker containerizer should not leak possibly sensitive data to agent log.


Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.0.4)

### Upgrades

Rolling upgrades from a Mesos 1.0.0 cluster to Mesos 1.0.4 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.0.4.

### Try it out

We encourage you to try out this release and let us know what you think. If you run into any issues, please let us know on the [user mailing ld Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 8 contributors who made 1.0.4 possible

Alexander Rukletsov, Benjamin Bannier, James Peach, Jie Yu, Kapil Arya, Neil Conway, Till Toenshoff, Vinod Kone.
