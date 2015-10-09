---
layout: post
title: Mesos 0.16.0 Released
permalink: /blog/mesos-0-16-0-released/
published: true
post_author:
  display_name: Yan Xu
  twitter: xujyan
tags: Release, HighAvailability
---

We recently released Mesos v0.16.0 on our [downloads](http://mesos.apache.org/downloads/) page. It includes major [refactoring work](https://issues.apache.org/jira/browse/MESOS-496) of the leading master election and detection process. This improves the reliability and flexibility of running multiple masters in your cluster, which provides Mesos with high availability.

In high availability mode, if a leading master machine fails, Mesos holds elections to determine a new leader. Slave machines and schedulers detect the new leading master and connect to it, without disrupting services running on Mesos. Leader election implementation details, including how it works with [Zookeeper](http://zookeeper.apache.org), are detailed in the [high availablity documentation](http://mesos.apache.org/documentation/latest/high-availability/).

## What's Changed
Aside from the refactoring, v0.16.0 includes fixes for bugs which caused incorrect termination of Mesos masters and slaves:

 * Fixed ZooKeeper related bugs which terminated Mesos processes instead of automatically retrying them: [MESOS-463](https://issues.apache.org/jira/browse/MESOS-463), [MESOS-465](https://issues.apache.org/jira/browse/MESOS-465), [MESOS-814](https://issues.apache.org/jira/browse/MESOS-814).
 *  Non-leading Master now stays up after ZooKeeper session expiration or after it is partitioned from ZooKeeper.
 * Slave no longer attempts to recover checkpointed data after a reboot: [MESOS-844](https://issues.apache.org/jira/browse/MESOS-844).

Click to read the full [release notes](https://issues.apache.org/jira/secure/ReleaseNote.jspa?projectId=12311242&version=12325295).

## Upgrading
To upgrade a live cluster, please refer to the [Upgrades document](http://mesos.apache.org/documentation/latest/upgrades/).

## Getting Involved

We encourage you to try out this release, and let us know what you think on the [user mailing list](mailto:user@mesos.apache.org). You can also get in touch with us via [@ApacheMesos](https://twitter.com/intent/user?screen_name=ApacheMesos) or via [mailing lists and IRC](https://mesos.apache.org/community).