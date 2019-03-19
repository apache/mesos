---
layout: post
title: Apache Mesos 1.5.3 Released
permalink: /blog/mesos-1-5-3-released/
published: true
post_author:
  display_name: Gilbert Song
  gravatar: 05d3596cf7ef7751c02545b1eaac64ac
  twitter: Gilbert_Songs
tags: Release
---

The latest Mesos 1.5.x release, 1.5.3, is now available for [download](http://mesos.apache.org/downloads). This release includes important bug fixes and improvements on top of 1.5.0. It is recommended to use this version if you are considering using Mesos 1.5. More specifically, this release includes the following:

* [MESOS-7474](https://issues.apache.org/jira/browse/MESOS-7474) - Mesos fetcher cache doesn't retry when missed.
* [MESOS-8887](https://issues.apache.org/jira/browse/MESOS-8887) - Unreachable tasks are not GC'ed when unreachable agent is GC'ed.
* [MESOS-8907](https://issues.apache.org/jira/browse/MESOS-8907) - Docker image fetcher fails with HTTP/2.
* [MESOS-9210](https://issues.apache.org/jira/browse/MESOS-9210) - Mesos v1 scheduler library does not properly handle SUBSCRIBE retries.
* [MESOS-9305](https://issues.apache.org/jira/browse/MESOS-9305) - Create cgoup recursively to workaround systemd deleting cgroups_root.
* [MESOS-9317](https://issues.apache.org/jira/browse/MESOS-9317) - Some master endpoints do not handle failed authorization properly.
* [MESOS-9332](https://issues.apache.org/jira/browse/MESOS-9332) - Nested container should run as the same user of its parent container by default.
* [MESOS-9334](https://issues.apache.org/jira/browse/MESOS-9334) - Container stuck at ISOLATING state due to libevent poll never returns.
* [MESOS-9362](https://issues.apache.org/jira/browse/MESOS-9362) - Test `CgroupsIsolatorTest.ROOT_CGROUPS_CreateRecursively` is flaky.
* [MESOS-9411](https://issues.apache.org/jira/browse/MESOS-9411) - Validation of JWT tokens using HS256 hashing algorithm is not thread safe.
* [MESOS-9492](https://issues.apache.org/jira/browse/MESOS-9492) - Persist CNI working directory across reboot.
* [MESOS-9507](https://issues.apache.org/jira/browse/MESOS-9507) - Agent could not recover due to empty docker volume checkpointed files.
* [MESOS-9518](https://issues.apache.org/jira/browse/MESOS-9518) - CNI_NETNS should not be set for orphan containers that do not have network namespace.
* [MESOS-9532](https://issues.apache.org/jira/browse/MESOS-9532) - ResourceOffersTest.ResourceOfferWithMultipleSlaves is flaky.
* [MESOS-9533](https://issues.apache.org/jira/browse/MESOS-9533) - CniIsolatorTest.ROOT_CleanupAfterReboot is flaky.
* [MESOS-9555](https://issues.apache.org/jira/browse/MESOS-9555) - Allocator CHECK failure: reservationScalarQuantities.contains(role).
* [MESOS-9581](https://issues.apache.org/jira/browse/MESOS-9581) - Mesos package naming appears to be undeterministic.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.5.3)

### Upgrades

Rolling upgrades from a Mesos 1.5.0 cluster to Mesos 1.5.3 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.5.3 from 1.4.x, 1.3.x, or 1.2.x.

### Try it out

Please try out this release and let us know what you think. If you run into any issues, let us know on the [user mailing list and/or Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 7 contributors who made 1.5.3 possible:

Andrei Budnik, Benjamin Mahler, Gilbert Song, Jie Yu, Qian Zhang, Till Toenshoff, Vinod Kone
