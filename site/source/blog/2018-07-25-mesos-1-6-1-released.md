---
layout: post
title: "Apache Mesos 1.6.1 Released"
published: true
post_author:
  display_name: Greg Mann
  gravatar: 20c5c9695f0891b6b093704331238133
  twitter: greggomann
tags: Release
---

Apache Mesos 1.6.1 is now available for [download](http://mesos.apache.org/downloads). This release includes important bug fixes and improvements to 1.6.0; if you're considering using Mesos 1.6, it's recommended that you use 1.6.1. The following notable fixes are included:

* [MESOS-3790](https://issues.apache.org/jira/browse/MESOS-3790) - ZooKeeper connection should retry on `EAI_NONAME`.
* [MESOS-8830](https://issues.apache.org/jira/browse/MESOS-8830) - Agent gc on old slave sandboxes could empty persistent volume data
* [MESOS-8871](https://issues.apache.org/jira/browse/MESOS-8871) - Agent may fail to recover if the agent dies before image store cache checkpointed.
* [MESOS-8904](https://issues.apache.org/jira/browse/MESOS-8904) - Master crash when removing quota.
* [MESOS-8936](https://issues.apache.org/jira/browse/MESOS-8936) - Implement a Random Sorter for offer allocations.
* [MESOS-8945](https://issues.apache.org/jira/browse/MESOS-8945) - Master check failure due to CHECK_SOME(providerId).
* [MESOS-8963](https://issues.apache.org/jira/browse/MESOS-8963) - Executor crash trying to print container ID.
* [MESOS-8980](https://issues.apache.org/jira/browse/MESOS-8980) - mesos-slave can deadlock with docker pull.
* [MESOS-8986](https://issues.apache.org/jira/browse/MESOS-8986) - `slave.available()` in the allocator is expensive and drags down allocation performance.
* [MESOS-8987](https://issues.apache.org/jira/browse/MESOS-8987) - Master asks agent to shutdown upon auth errors.
* [MESOS-9002](https://issues.apache.org/jira/browse/MESOS-9002) - GCC 8.1 build failure in os::Fork::Tree.
* [MESOS-9024](https://issues.apache.org/jira/browse/MESOS-9024) - Mesos master segfaults with stack overflow under load.
* [MESOS-9025](https://issues.apache.org/jira/browse/MESOS-9025) - The container which joins CNI network and has checkpoint enabled will be mistakenly destroyed by agent.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.6.1).

### Upgrades

Rolling upgrades from a Mesos 1.6.0 cluster to Mesos 1.6.1 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.6.1 from other versions.

### Try it out

Please try out this release and let us know what you think. If you run into any issues, let us know on the [user mailing list and/or Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 18 contributors who made 1.6.1 possible:

Alexander Rojas, Alexander Rukletsov, Andrew Schwartzmeyer, Armand Grillet, Benjamin Bannier, Benjamin Mahler, Benno Evers, bin zheng, Chun-Hung Hsiao, Gastón Kleiman, Gilbert Song, Greg Mann, he yi hua, James Peach, Jie Yu, Kjetil Joergensen, Meng Zhu, Zhitao Li
