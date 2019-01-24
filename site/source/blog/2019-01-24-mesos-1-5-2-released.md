---
layout: post
title: Apache Mesos 1.5.2 Released
permalink: /blog/mesos-1-5-2-released/
published: true
post_author:
  display_name: Gilbert Song
  gravatar: 05d3596cf7ef7751c02545b1eaac64ac
  twitter: Gilbert_Songs
tags: Release
---

The latest Mesos 1.5.x release, 1.5.2, is now available for [download](http://mesos.apache.org/downloads). This release includes important bug fixes and improvements on top of 1.5.0. It is recommended to use this version if you are considering using Mesos 1.5. More specifically, this release includes the following:

* [MESOS-3790](https://issues.apache.org/jira/browse/MESOS-3790) - ZooKeeper connection should retry on `EAI_NONAME`.
* [MESOS-7474](https://issues.apache.org/jira/browse/MESOS-7474) - Mesos fetcher cache doesn't retry when missed.
* [MESOS-8128](https://issues.apache.org/jira/browse/MESOS-8128) - Make os::pipe file descriptors O_CLOEXEC.
* [MESOS-8418](https://issues.apache.org/jira/browse/MESOS-8418) - mesos-agent high cpu usage because of numerous /proc/mounts reads.
* [MESOS-8545](https://issues.apache.org/jira/browse/MESOS-8545) - AgentAPIStreamingTest.AttachInputToNestedContainerSession is flaky.
* [MESOS-8568](https://issues.apache.org/jira/browse/MESOS-8568) - Command checks should always call `WAIT_NESTED_CONTAINER` before `REMOVE_NESTED_CONTAINER`.
* [MESOS-8620](https://issues.apache.org/jira/browse/MESOS-8620) - Containers stuck in FETCHING possibly due to unresponsive server.
* [MESOS-8830](https://issues.apache.org/jira/browse/MESOS-8830) - Agent gc on old slave sandboxes could empty persistent volume data.
* [MESOS-8871](https://issues.apache.org/jira/browse/MESOS-8871) - Agent may fail to recover if the agent dies before image store cache checkpointed.
* [MESOS-8904](https://issues.apache.org/jira/browse/MESOS-8904) - Master crash when removing quota.
* [MESOS-8906](https://issues.apache.org/jira/browse/MESOS-8906) - `UriDiskProfileAdaptor` fails to update profile selectors.
* [MESOS-8907](https://issues.apache.org/jira/browse/MESOS-8907) - Docker image fetcher fails with HTTP/2.
* [MESOS-8917](https://issues.apache.org/jira/browse/MESOS-8917) - Agent leaking file descriptors into forked processes.
* [MESOS-8921](https://issues.apache.org/jira/browse/MESOS-8921) - Autotools don't work with newer OpenJDK versions.
* [MESOS-8935](https://issues.apache.org/jira/browse/MESOS-8935) - Quota limit "chopping" can lead to cpu-only and memory-only offers.
* [MESOS-8936](https://issues.apache.org/jira/browse/MESOS-8936) - Implement a Random Sorter for offer allocations.
* [MESOS-8942](https://issues.apache.org/jira/browse/MESOS-8942) - Master streaming API does not send (health) check updates for tasks.
* [MESOS-8945](https://issues.apache.org/jira/browse/MESOS-8945) - Master check failure due to CHECK_SOME(providerId).
* [MESOS-8947](https://issues.apache.org/jira/browse/MESOS-8947) - Improve the container preparing logging in IOSwitchboard and volume/secret isolator.
* [MESOS-8952](https://issues.apache.org/jira/browse/MESOS-8952) - process::await/collect n^2 performance issue.
* [MESOS-8963](https://issues.apache.org/jira/browse/MESOS-8963) - Executor crash trying to print container ID.
* [MESOS-8978](https://issues.apache.org/jira/browse/MESOS-8978) - Command executor calling setsid breaks the tty support.
* [MESOS-8980](https://issues.apache.org/jira/browse/MESOS-8980) - mesos-slave can deadlock with docker pull.
* [MESOS-8986](https://issues.apache.org/jira/browse/MESOS-8986) - `slave.available()` in the allocator is expensive and drags down allocation performance.
* [MESOS-8987](https://issues.apache.org/jira/browse/MESOS-8987) - Master asks agent to shutdown upon auth errors.
* [MESOS-9024](https://issues.apache.org/jira/browse/MESOS-9024) - Mesos master segfaults with stack overflow under load.
* [MESOS-9049](https://issues.apache.org/jira/browse/MESOS-9049) - Agent GC could unmount a dangling persistent volume multiple times.
* [MESOS-9116](https://issues.apache.org/jira/browse/MESOS-9116) - Launch nested container session fails due to incorrect detection of `mnt` namespace of command executor's task.
* [MESOS-9125](https://issues.apache.org/jira/browse/MESOS-9125) - Port mapper CNI plugin might fail with "Resource temporarily unavailable".
* [MESOS-9127](https://issues.apache.org/jira/browse/MESOS-9127) - Port mapper CNI plugin might deadlock iptables on the agent.
* [MESOS-9131](https://issues.apache.org/jira/browse/MESOS-9131) - Health checks launching nested containers while a container is being destroyed lead to unkillable tasks.
* [MESOS-9142](https://issues.apache.org/jira/browse/MESOS-9142) - CNI detach might fail due to missing network config file.
* [MESOS-9144](https://issues.apache.org/jira/browse/MESOS-9144) - Master authentication handling leads to request amplification.
* [MESOS-9145](https://issues.apache.org/jira/browse/MESOS-9145) - Master has a fragile burned-in 5s authentication timeout.
* [MESOS-9146](https://issues.apache.org/jira/browse/MESOS-9146) - Agent has a fragile burn-in 5s authentication timeout.
* [MESOS-9147](https://issues.apache.org/jira/browse/MESOS-9147) - Agent and scheduler driver authentication retry backoff time could overflow.
* [MESOS-9151](https://issues.apache.org/jira/browse/MESOS-9151) - Container stuck at ISOLATING due to FD leak.
* [MESOS-9170](https://issues.apache.org/jira/browse/MESOS-9170) - Zookeeper doesn't compile with newer gcc due to format error.
* [MESOS-9196](https://issues.apache.org/jira/browse/MESOS-9196) - Removing rootfs mounts may fail with EBUSY.
* [MESOS-9231](https://issues.apache.org/jira/browse/MESOS-9231) - `docker inspect` may return an unexpected result to Docker executor due to a race condition.
* [MESOS-9267](https://issues.apache.org/jira/browse/MESOS-9267) - Mesos agent crashes when CNI network is not configured but used.
* [MESOS-9279](https://issues.apache.org/jira/browse/MESOS-9279) - Docker Containerizer 'usage' call might be expensive if mount table is big.
* [MESOS-9283](https://issues.apache.org/jira/browse/MESOS-9283) - Docker containerizer actor can get backlogged with large number of containers.
* [MESOS-9305](https://issues.apache.org/jira/browse/MESOS-9305) - Create cgoup recursively to workaround systemd deleting cgroups_root.
* [MESOS-9308](https://issues.apache.org/jira/browse/MESOS-9308) - URI disk profile adaptor could deadlock.
* [MESOS-9317](https://issues.apache.org/jira/browse/MESOS-9317) - Some master endpoints do not handle failed authorization properly.
* [MESOS-9332](https://issues.apache.org/jira/browse/MESOS-9332) - Nested container should run as the same user of its parent container by default.
* [MESOS-9334](https://issues.apache.org/jira/browse/MESOS-9334) - Container stuck at ISOLATING state due to libevent poll never returns.
* [MESOS-9411](https://issues.apache.org/jira/browse/MESOS-9411) - Validation of JWT tokens using HS256 hashing algorithm is not thread safe.
* [MESOS-9419](https://issues.apache.org/jira/browse/MESOS-9419) - Executor to framework message crashes master if framework has not re-registered.
* [MESOS-9480](https://issues.apache.org/jira/browse/MESOS-9480) - Master may skip processing authorization results for `LAUNCH_GROUP`.
* [MESOS-9492](https://issues.apache.org/jira/browse/MESOS-9492) - Persist CNI working directory across reboot.
* [MESOS-9501](https://issues.apache.org/jira/browse/MESOS-9501) - Mesos executor fails to terminate and gets stuck after agent host reboot.
* [MESOS-9502](https://issues.apache.org/jira/browse/MESOS-9502) - IOswitchboard cleanup could get stuck due to FD leak from a race.
* [MESOS-9510](https://issues.apache.org/jira/browse/MESOS-9510) - Disallowed nan, inf and so on in `Value::Scalar`.
* [MESOS-9516](https://issues.apache.org/jira/browse/MESOS-9516) - Extend `min_allocatable_resources` flag to cover non-scalar resources.
* [MESOS-9518](https://issues.apache.org/jira/browse/MESOS-9518) - CNI_NETNS should not be set for orphan containers that do not have network namespace.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.5.2)

### Upgrades

Rolling upgrades from a Mesos 1.5.0 cluster to Mesos 1.5.2 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.5.2 from 1.4.x, 1.3.x, or 1.2.x.

### Try it out

Please try out this release and let us know what you think. If you run into any issues, let us know on the [user mailing list and/or Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 29 contributors who made 1.5.2 possible:

Alexander Rojas, Alexander Rukletsov, Andrei Budnik, Andrew Schwartzmeyer, Armand Grillet, Benjamin Bannier, Benjamin Mahler, Benno Evers, Chun-Hung Hsiao, Deepak Goel, Gastón Kleiman, Gilbert Song, Greg Mann, James Peach, Jie Yu, Joseph Wu, Kapil Arya, Kevin Klues, Kjetil Joergensen, Meng Zhu, Michael Park, Qian Zhang, Radhika Jandhyala, Till Toenshoff, Vinod Kone, Zhitao Li, bin zheng, fei long, he yi hua
