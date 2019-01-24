---
layout: post
title: Apache Mesos 1.5.1 Released
permalink: /blog/mesos-1-5-1-released/
published: true
post_author:
  display_name: Gilbert Song
  gravatar: 05d3596cf7ef7751c02545b1eaac64ac
  twitter: Gilbert_Songs
tags: Release
---

The latest Mesos 1.5.x release, 1.5.1, is now available for [download](http://mesos.apache.org/downloads). This release includes important bug fixes and improvements on top of 1.5.0. It is recommended to use this version if you are considering using Mesos 1.5. More specifically, this release includes the following:

* [MESOS-1720](https://issues.apache.org/jira/browse/MESOS-1720) - Slave should send exited executor message when the executor is never launched.
* [MESOS-7742](https://issues.apache.org/jira/browse/MESOS-7742) - Race conditions in IOSwitchboard: listening on unix socket and premature closing of the connection.
* [MESOS-8125](https://issues.apache.org/jira/browse/MESOS-8125) - Agent should properly handle recovering an executor when its pid is reused.
* [MESOS-8411](https://issues.apache.org/jira/browse/MESOS-8411) - Killing a queued task can lead to the command executor never terminating.
* [MESOS-8416](https://issues.apache.org/jira/browse/MESOS-8416) - CHECK failure if trying to recover nested containers but the framework checkpointing is not enabled.
* [MESOS-8468](https://issues.apache.org/jira/browse/MESOS-8468) - `LAUNCH_GROUP` failure tears down the default executor.
* [MESOS-8488](https://issues.apache.org/jira/browse/MESOS-8488) - Docker bug can cause unkillable tasks.
* [MESOS-8510](https://issues.apache.org/jira/browse/MESOS-8510) - URI disk profile adaptor does not consider plugin type for a profile.
* [MESOS-8536](https://issues.apache.org/jira/browse/MESOS-8536) - Pending offer operations on resource provider resources not properly accounted for in allocator.
* [MESOS-8550](https://issues.apache.org/jira/browse/MESOS-8550) - Bug in `Master::detected()` leads to coredump in `MasterZooKeeperTest.MasterInfoAddress`.
* [MESOS-8552](https://issues.apache.org/jira/browse/MESOS-8552) - CGROUPS_ROOT_PidNamespaceForward and CGROUPS_ROOT_PidNamespaceBackward tests fail.
* [MESOS-8565](https://issues.apache.org/jira/browse/MESOS-8565) - Persistent volumes are not visible in Mesos UI when launching a pod using default executor.
* [MESOS-8569](https://issues.apache.org/jira/browse/MESOS-8569) - Allow newline characters when decoding base64 strings in stout.
* [MESOS-8574](https://issues.apache.org/jira/browse/MESOS-8574) - Docker executor makes no progress when 'docker inspect' hangs.
* [MESOS-8575](https://issues.apache.org/jira/browse/MESOS-8575) - Improve discard handling for 'Docker::stop' and 'Docker::pull'.
* [MESOS-8576](https://issues.apache.org/jira/browse/MESOS-8576) - Improve discard handling of 'Docker::inspect()'.
* [MESOS-8577](https://issues.apache.org/jira/browse/MESOS-8577) - Destroy nested container if `LAUNCH_NESTED_CONTAINER_SESSION` fails.
* [MESOS-8594](https://issues.apache.org/jira/browse/MESOS-8594) - Mesos master stack overflow in libprocess socket send loop.
* [MESOS-8598](https://issues.apache.org/jira/browse/MESOS-8598) - Allow empty resource provider selector in `UriDiskProfileAdaptor`.
* [MESOS-8601](https://issues.apache.org/jira/browse/MESOS-8601) - Master crashes during slave reregistration after failover.
* [MESOS-8604](https://issues.apache.org/jira/browse/MESOS-8604) - Quota headroom tracking may be incorrect in the presence of hierarchical reservation.
* [MESOS-8605](https://issues.apache.org/jira/browse/MESOS-8605) - Terminal task status update will not send if 'docker inspect' is hung.
* [MESOS-8619](https://issues.apache.org/jira/browse/MESOS-8619) - Docker on Windows uses `USERPROFILE` instead of `HOME` for credentials.
* [MESOS-8624](https://issues.apache.org/jira/browse/MESOS-8624) - Valid tasks may be explicitly dropped by agent due to race conditions.
* [MESOS-8631](https://issues.apache.org/jira/browse/MESOS-8631) - Agent should be able to start a task with every CPU on a Windows machine.
* [MESOS-8641](https://issues.apache.org/jira/browse/MESOS-8641) - Event stream could send heartbeat before subscribed.
* [MESOS-8646](https://issues.apache.org/jira/browse/MESOS-8646) - Agent should be able to resolve file names on open files.
* [MESOS-8651](https://issues.apache.org/jira/browse/MESOS-8651) - Potential memory leaks in the `volume/sandbox_path` isolator.
* [MESOS-8741](https://issues.apache.org/jira/browse/MESOS-8741) - `Add` to sequence will not run if it races with sequence destruction.
* [MESOS-8742](https://issues.apache.org/jira/browse/MESOS-8742) - Agent resource provider config API calls should be idempotent.
* [MESOS-8786](https://issues.apache.org/jira/browse/MESOS-8786) - CgroupIsolatorProcess accesses subsystem processes directly.
* [MESOS-8787](https://issues.apache.org/jira/browse/MESOS-8787) - RP-related API should be experimental.
* [MESOS-8876](https://issues.apache.org/jira/browse/MESOS-8876) - Normal exit of Docker container using rexray volume results in TASK_FAILED.
* [MESOS-8881](https://issues.apache.org/jira/browse/MESOS-8881) - Enable epoll backend in libevent integration.
* [MESOS-8885](https://issues.apache.org/jira/browse/MESOS-8885) - Disable libevent debug mode.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.5.1)

### Upgrades

Rolling upgrades from a Mesos 1.5.0 cluster to Mesos 1.5.1 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.5.1 from 1.4.x, 1.3.x, or 1.2.x.

### Try it out

Please try out this release and let us know what you think. If you run into any issues, let us know on the [user mailing list and/or Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 23 contributors who made 1.5.1 possible:

Akash Gupta, Alexander Rukletsov, Andrei Budnik, Andrew Schwartzmeyer, Benjamin Bannier, Benjamin Mahler, Benno Evers, Chun-Hung Hsiao, Gaston Kleiman, Gilbert Song, Greg Mann, Ilya Pronin, James Peach, Jason Lai, Jie Yu, John Kordich, Kapil Arya, Meng Zhu, Michael Park, Qian Zhang, Tomasz Janiszewski, Vinod Kone, Zhitao Li
