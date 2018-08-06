---
layout: post
title: Apache Mesos 0.27.2 Released
permalink: /blog/mesos-0-27-2-released/
published: true
post_author:
  display_name: Michael Park
  gravatar: 2ab9cab3a7cf782261c583c1f48a81b0
  twitter: mcypark
tags: Release
---

The latest Mesos release, 0.27.2, is now available for [download](http://mesos.apache.org/downloads).
This release includes fixes and improvements for the following:

* [MESOS-4693](https://issues.apache.org/jira/browse/MESOS-4693) - Variable shadowing in HookManager::slavePreLaunchDockerHook.
* [MESOS-4711](https://issues.apache.org/jira/browse/MESOS-4711) - Race condition in libevent poll implementation causes crash.
* [MESOS-4754](https://issues.apache.org/jira/browse/MESOS-4754) - The "executors" field is exposed under a backwards incompatible schema.
* [MESOS-4687](https://issues.apache.org/jira/browse/MESOS-4687) - Implement reliable floating point for scalar resources.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=0.27.2).

### Upgrades

Rolling upgrades from a Mesos 0.27.1 cluster to Mesos 0.27.2 are straightforward.
Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 0.27.2.


### Try it out

We encourage you to try out this release and let us know what you think.
If you run into any issues, please let us know on the [user mailing list and IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 4 contributors who made 0.27.2 possible:

Alexander Rojas, Kevin Devroede, Michael Park, Neil Conway
