---
layout: post
title: Apache Mesos 1.4.1 Released
permalink: /blog/mesos-1-4-1-released/
published: true
post_author:
  display_name: Kapil Arya
tags: Release
---

The latest Mesos 1.4.x release, 1.4.1, is now available for [download](http://mesos.apache.org/downloads). This release includes important bug fixes and improvements on top of 1.4.0. It is recommended to use this version if you are considering using Mesos 1.4. More specifically, this release includes the following:

* [MESOS-7873](https://issues.apache.org/jira/browse/MESOS-7873) - Expose `ExecutorInfo.ContainerInfo.NetworkInfo` in Mesos `state` endpoint.
* [MESOS-7921](https://issues.apache.org/jira/browse/MESOS-7921) - ProcessManager::resume sometimes crashes accessing EventQueue.
* [MESOS-7964](https://issues.apache.org/jira/browse/MESOS-7964) - Heavy-duty GC makes the agent unresponsive.
* [MESOS-7968](https://issues.apache.org/jira/browse/MESOS-7968) - Handle `/proc/self/ns/pid_for_children` when parsing available namespace.
* [MESOS-7969](https://issues.apache.org/jira/browse/MESOS-7969) - Handle cgroups v2 hierarchy when parsing /proc/self/cgroups.
* [MESOS-7980](https://issues.apache.org/jira/browse/MESOS-7980) - Stout fails to compile with libc >= 2.26.
* [MESOS-8051](https://issues.apache.org/jira/browse/MESOS-8051) - Killing TASK_GROUP fail to kill some tasks.
* [MESOS-8080](https://issues.apache.org/jira/browse/MESOS-8080) - The default executor does not propagate missing task exit status correctly.
* [MESOS-8090](https://issues.apache.org/jira/browse/MESOS-8090) - Mesos 1.4.0 crashes with 1.3.x agent with oversubscription
* [MESOS-8135](https://issues.apache.org/jira/browse/MESOS-8135) - Masters can lose track of tasks' executor IDs.
* [MESOS-8169](https://issues.apache.org/jira/browse/MESOS-8169) - Incorrect master validation forces executor IDs to be globally unique.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.4.1)

### Upgrades

Rolling upgrades from a Mesos 1.4.0 cluster to Mesos 1.4.1 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.4.1 from 1.0.x, 1.2.x, or 1.3.x.

### Try it out

Please try out this release and let us know what you think. If you run into any issues, let us know on the [user mailing list and/or Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 11 contributors who made 1.4.1 possible:

Benjamin Mahler, Chun-Hung Hsiao, Deepak Goel, Gaston Kleiman, James DeFelice, James Peach, Jiang Yan Xu, Kapil Arya, Michael Park, Qian Zhang, Zhitao Li
