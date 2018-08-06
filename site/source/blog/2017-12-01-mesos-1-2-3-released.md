---
layout: post
title: Apache Mesos 1.2.3 Released
permalink: /blog/mesos-1-2-3-released/
published: true
post_author:
  display_name: Adam B
tags: Release
---

The latest (and final) Mesos 1.2.x release, 1.2.3, is now available for [download](http://mesos.apache.org/downloads). This release includes important bug fixes on top of 1.2.2. It is recommended to use this version if you are considering using Mesos 1.2. More specifically, this release includes the following bug fixes:

* [MESOS-6743](https://issues.apache.org/jira/browse/MESOS-6743) - Docker executor hangs forever if `docker stop` fails.
* [MESOS-6950](https://issues.apache.org/jira/browse/MESOS-6950) - Launching two tasks with the same Docker image simultaneously may cause a staging dir never cleaned up.
* [MESOS-7365](https://issues.apache.org/jira/browse/MESOS-7365) - Compile error with recent glibc.
* [MESOS-7378](https://issues.apache.org/jira/browse/MESOS-7378) - Build failure with glibc 2.12.
* [MESOS-7627](https://issues.apache.org/jira/browse/MESOS-7627) - Mesos slave stucks.
* [MESOS-7652](https://issues.apache.org/jira/browse/MESOS-7652) - Docker image with universal containerizer does not work if WORKDIR is missing in the rootfs.
* [MESOS-7744](https://issues.apache.org/jira/browse/MESOS-7744) - Mesos Agent Sends TASK_KILL status update to Master, and still launches task.
* [MESOS-7783](https://issues.apache.org/jira/browse/MESOS-7783) - Framework might not receive status update when a just launched task is killed immediately.
* [MESOS-7858](https://issues.apache.org/jira/browse/MESOS-7858) - Launching a nested container with namespace/pid isolation, with glibc < 2.25, may deadlock the LinuxLauncher and MesosContainerizer.
* [MESOS-7863](https://issues.apache.org/jira/browse/MESOS-7863) - Agent may drop pending kill task status updates.
* [MESOS-7865](https://issues.apache.org/jira/browse/MESOS-7865) - Agent may process a kill task and still launch the task.
* [MESOS-7872](https://issues.apache.org/jira/browse/MESOS-7872) - Scheduler hang when registration fails.
* [MESOS-7909](https://issues.apache.org/jira/browse/MESOS-7909) - Ordering dependency between 'linux/capabilities' and 'docker/runtime' isolator.
* [MESOS-7926](https://issues.apache.org/jira/browse/MESOS-7926) - Abnormal termination of default executor can cause MesosContainerizer::destroy to fail.
* [MESOS-7934](https://issues.apache.org/jira/browse/MESOS-7934) - OOM due to LibeventSSLSocket send incorrectly returning 0 after shutdown.
* [MESOS-7968](https://issues.apache.org/jira/browse/MESOS-7968) - Handle `/proc/self/ns/pid_for_children` when parsing available namespace.
* [MESOS-7969](https://issues.apache.org/jira/browse/MESOS-7969) - Handle cgroups v2 hierarchy when parsing /proc/self/cgroups.
* [MESOS-7975](https://issues.apache.org/jira/browse/MESOS-7975) - The command/default/docker executor can incorrectly send a TASK_FINISHED update even when the task is killed.
* [MESOS-7980](https://issues.apache.org/jira/browse/MESOS-7980) - Stout fails to compile with libc >= 2.26.
* [MESOS-8051](https://issues.apache.org/jira/browse/MESOS-8051) - Killing TASK_GROUP fail to kill some tasks.
* [MESOS-8080](https://issues.apache.org/jira/browse/MESOS-8080) - The default executor does not propagate missing task exit status correctly.
* [MESOS-8135](https://issues.apache.org/jira/browse/MESOS-8135) - Masters can lose track of tasks' executor IDs.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.2.3)

### Upgrades

Rolling upgrades from a Mesos 1.2.2 cluster to Mesos 1.2.3 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.2.3 from 1.1.x or 1.0.x.

NOTE: Since Mesos 1.2.1, the master does not allow 0.x agents to register.

### Try it out

Please try out this release and let us know what you think. If you run into any issues, let us know on the [user mailing list and/or Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 12 contributors who made 1.2.3 possible:

Adam B, Alexander Rukletsov, Andrei Budnik, Benjamin Hindman, Benjamin Mahler, Gaston Kleiman, Gilbert Song, James Peach, Jie Yu, Kapil Arya, Neil Conway, and Qian Zhang.
