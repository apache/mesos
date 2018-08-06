---
layout: post
title: Apache Mesos 1.1.3 Released
permalink: /blog/mesos-1-1-3-released/
published: true
post_author:
  display_name: AlexR
tags: Release
---

The latest Mesos release, 1.1.3, is now available for
[download](http://mesos.apache.org/downloads). This release includes some
important bug fixes and improvements on top of 1.1.2. It is highly recommended
that users use this version if you are considering using Mesos 1.1. More
specifically, this release includes the following major fixes and improvements:

* [MESOS-5187](https://issues.apache.org/jira/browse/MESOS-5187) - The filesystem/linux isolator does not set the permissions of the host_path.
* [MESOS-6743](https://issues.apache.org/jira/browse/MESOS-6743) - Docker executor hangs forever if `docker stop` fails.
* [MESOS-6950](https://issues.apache.org/jira/browse/MESOS-6950) - Launching two tasks with the same Docker image simultaneously may cause a staging dir never cleaned up.
* [MESOS-7540](https://issues.apache.org/jira/browse/MESOS-7540) - Add an agent flag for executor re-registration timeout.
* [MESOS-7569](https://issues.apache.org/jira/browse/MESOS-7569) - Allow "old" executors with half-open connections to be preserved during agent upgrade / restart.
* [MESOS-7689](https://issues.apache.org/jira/browse/MESOS-7689) - Libprocess can crash on malformed request paths for libprocess messages.
* [MESOS-7690](https://issues.apache.org/jira/browse/MESOS-7690) - The agent can crash when an unknown executor tries to register.
* [MESOS-7581](https://issues.apache.org/jira/browse/MESOS-7581) - Fix interference of external Boost installations when using some unbundled dependencies.
* [MESOS-7703](https://issues.apache.org/jira/browse/MESOS-7703) - Mesos fails to exec a custom executor when no shell is used.
* [MESOS-7728](https://issues.apache.org/jira/browse/MESOS-7728) - Java HTTP adapter crashes JVM when leading master disconnects.
* [MESOS-7770](https://issues.apache.org/jira/browse/MESOS-7770) - Persistent volume might not be mounted if there is a sandbox volume whose source is the same as the target of the persistent volume.
* [MESOS-7777](https://issues.apache.org/jira/browse/MESOS-7777) - Agent failed to recover due to mount namespace leakage in Docker 1.12/1.13.
* [MESOS-7796](https://issues.apache.org/jira/browse/MESOS-7796) - LIBPROCESS_IP isn't passed on to the fetcher.
* [MESOS-7830](https://issues.apache.org/jira/browse/MESOS-7830) - Sandbox_path volume does not have ownership set correctly.
* [MESOS-7863](https://issues.apache.org/jira/browse/MESOS-7863) - Agent may drop pending kill task status updates.
* [MESOS-7865](https://issues.apache.org/jira/browse/MESOS-7865) - Agent may process a kill task and still launch the task.

Full release notes are available in the release
[CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.1.3)

### Upgrades

Rolling upgrades from a Mesos 1.1.2 cluster to Mesos 1.1.3 are straightforward.
Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/)
for detailed information on upgrading to Mesos 1.1.3.

### Try it out

We encourage you to try out this release and let us know what you think. If you
run into any issues, please let us know on the
[user mailing list and IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 11 contributors who made 1.1.3 possible:

Aaron Wood, Alexander Rukletsov, Andrei Budnik, Benjamin Bannier, Benjamin Mahler, Chun-Hung Hsiao, Gastón Kleiman, Gilbert Song, Greg Mann, Jie Yu, and Qian Zhang.
