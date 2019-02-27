---
layout: post
title: Apache Mesos 1.4.3 Released
permalink: /blog/mesos-1-4-3-released/
published: true
post_author:
  display_name: Meng Zhu
tags: Release
---

The latest Mesos 1.4.x release, 1.4.3, is now available for [download](http://mesos.apache.org/downloads). This release includes important bug fixes and improvements on top of 1.4.2. It is recommended to use this version if you are considering using Mesos 1.4. More specifically, this release includes the following notable fixes:

* [MESOS-8128](https://issues.apache.org/jira/browse/MESOS-8128) - Make os::pipe file descriptors O_CLOEXEC.
* [MESOS-8568](https://issues.apache.org/jira/browse/MESOS-8568) - Command checks should always call `WAIT_NESTED_CONTAINER` before `REMOVE_NESTED_CONTAINER`
* [MESOS-8620](https://issues.apache.org/jira/browse/MESOS-8620) - Containers stuck in FETCHING possibly due to unresponsive server.
* [MESOS-8917](https://issues.apache.org/jira/browse/MESOS-8917) - Agent leaking file descriptors into forked processes.
* [MESOS-8921](https://issues.apache.org/jira/browse/MESOS-8921) - Autotools don't work with newer OpenJDK versions
* [MESOS-9144](https://issues.apache.org/jira/browse/MESOS-9144) - Master authentication handling leads to request amplification.
* [MESOS-9145](https://issues.apache.org/jira/browse/MESOS-9145) - Master has a fragile burned-in 5s authentication timeout.
* [MESOS-9146](https://issues.apache.org/jira/browse/MESOS-9146) - Agent has a fragile burn-in 5s authentication timeout.
* [MESOS-9147](https://issues.apache.org/jira/browse/MESOS-9147) - Agent and scheduler driver authentication retry backoff time could overflow.
* [MESOS-9151](https://issues.apache.org/jira/browse/MESOS-9151) - Container stuck at ISOLATING due to FD leak.
* [MESOS-9170](https://issues.apache.org/jira/browse/MESOS-9170) - Zookeeper doesn't compile with newer gcc due to format error.
* [MESOS-9196](https://issues.apache.org/jira/browse/MESOS-9196) - Removing rootfs mounts may fail with EBUSY.
* [MESOS-9221](https://issues.apache.org/jira/browse/MESOS-9221) - If some image layers are large, the image pulling may stuck due to the authorized token expired.
* [MESOS-9231](https://issues.apache.org/jira/browse/MESOS-9231) - `docker inspect` may return an unexpected result to Docker executor due to a race condition.
* [MESOS-9279](https://issues.apache.org/jira/browse/MESOS-9279) - Docker Containerizer 'usage' call might be expensive if mount table is big.
* [MESOS-9283](https://issues.apache.org/jira/browse/MESOS-9283) - Docker containerizer actor can get backlogged with large number of containers.
* [MESOS-9304](https://issues.apache.org/jira/browse/MESOS-9304) - Test `CGROUPS_ROOT_PidNamespaceForward` and `CGROUPS_ROOT_PidNamespaceBackward` fails on 1.4.x.
* [MESOS-9334](https://issues.apache.org/jira/browse/MESOS-9334) - Container stuck at ISOLATING state due to libevent poll never returns.
* [MESOS-9419](https://issues.apache.org/jira/browse/MESOS-9419) - Executor to framework message crashes master if framework has not re-registered.
* [MESOS-9480](https://issues.apache.org/jira/browse/MESOS-9480) - Master may skip processing authorization results for `LAUNCH_GROUP`.
* [MESOS-9492](https://issues.apache.org/jira/browse/MESOS-9492) - Persist CNI working directory across reboot.
* [MESOS-9501](https://issues.apache.org/jira/browse/MESOS-9501) - Mesos executor fails to terminate and gets stuck after agent host reboot.
* [MESOS-9502](https://issues.apache.org/jira/browse/MESOS-9502) - IOswitchboard cleanup could get stuck due to FD leak from a race.
* [MESOS-9518](https://issues.apache.org/jira/browse/MESOS-9518) - CNI_NETNS should not be set for orphan containers that do not have network namespace.
* [MESOS-9532](https://issues.apache.org/jira/browse/MESOS-9532) - ResourceOffersTest.ResourceOfferWithMultipleSlaves is flaky.
* [MESOS-9533](https://issues.apache.org/jira/browse/MESOS-9533) - CniIsolatorTest.ROOT_CleanupAfterReboot is flaky.
* [MESOS-9510](https://issues.apache.org/jira/browse/MESOS-9510) - Disallowed nan, inf and so on in `Value::Scalar`.
* [MESOS-9516](https://issues.apache.org/jira/browse/MESOS-9516) - Extend `min_allocatable_resources` flag to cover non-scalar resources.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.4.3)

### Upgrades

Rolling upgrades from a Mesos 1.4.2 cluster to Mesos 1.4.3 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.4.2 from 1.0.x, 1.2.x, or 1.3.x.

### Try it out

Please try out this release and let us know what you think. If you run into any issues, let us know on the [user mailing list and/or Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 18 contributors who made 1.4.3 possible:

Alexander Rukletsov, Andrei Budnik, Andrew Schwartzmeyer, Benjamin Bannier, Benjamin Mahler, Benno Evers, Chun-Hung Hsiao, Deepak Goel, Gastón Kleiman, Gilbert Song, Greg Mann, James Peach, Jie Yu, Kapil Arya, Meng Zhu, Michael Park, Qian Zhang, Radhika Jandhyala
