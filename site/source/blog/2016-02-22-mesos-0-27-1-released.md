---
layout: post
title: Apache Mesos 0.27.1 Released
permalink: /blog/mesos-0-27-1-released/
published: true
post_author:
  display_name: Michael Park
  gravatar: 2ab9cab3a7cf782261c583c1f48a81b0
  twitter: mcypark
tags: Release
---

The latest Mesos release, 0.27.1, is now available for [download](http://mesos.apache.org/downloads).
This release includes fixes and improvements for: reconnection logic for Zookeeper client, `/state` endpoint and `systemd` integration.

* [MESOS-4546](https://issues.apache.org/jira/browse/MESOS-4546) - Mesos Agents needs to re-resolve hosts in zk string on leader change / failure to connect.
* [MESOS-4582](https://issues.apache.org/jira/browse/MESOS-4582) - state.json serving duplicate "active" fields.
* [MESOS-3007](https://issues.apache.org/jira/browse/MESOS-3007) - Support systemd with Mesos.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=0.27.1).

### Upgrades

Rolling upgrades from a Mesos 0.27.0 cluster to Mesos 0.27.1 are straightforward.
Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 0.27.1.


### Try it out

We encourage you to try out this release and let us know what you think.
If you run into any issues, please let us know on the [user mailing list and IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 8 contributors who made 0.27.1 possible:

Jie Yu, Joerg Shad, Joris Van Remoortere, Joseph Wu, Kapil Arya, Michael Park, Neil Conway, Shuai Lin
