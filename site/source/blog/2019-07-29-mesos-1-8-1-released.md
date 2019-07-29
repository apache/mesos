---
layout: post
title: Apache Mesos 1.8.1 Released
permalink: /blog/mesos-1-8-1-released/
published: true
post_author:
  display_name: Benno Evers
tags: Release
---

The latest Mesos 1.8.x release, 1.8.1, is now available for [download](http://mesos.apache.org/downloads). This release includes important bug fixes and improvements on top of 1.8.0. It is recommended to use this version if you are considering using Mesos 1.8. More specifically, this release includes the following:

  * [MESOS-9395](https://issues.apache.org/jira/browse/MESOS-9395) - Check failure on `StorageLocalResourceProviderProcess::applyCreateDisk`.
  * [MESOS-9616](https://issues.apache.org/jira/browse/MESOS-9616) - `Filters.refuse_seconds` declines resources not in offers.
  * [MESOS-9730](https://issues.apache.org/jira/browse/MESOS-9730) - Executors cannot reconnect with agents using TLS1.3
  * [MESOS-9750](https://issues.apache.org/jira/browse/MESOS-9750) - Agent V1 `GET_STATE` response may report a complete executor's tasks as non-terminal after a graceful agent shutdown.
  * [MESOS-9766](https://issues.apache.org/jira/browse/MESOS-9766) - /__processes__ endpoint can hang.
  * [MESOS-9779](https://issues.apache.org/jira/browse/MESOS-9779) - `UPDATE_RESOURCE_PROVIDER_CONFIG` agent call returns 404 ambiguously.
  * [MESOS-9782](https://issues.apache.org/jira/browse/MESOS-9782) - Random sorter fails to clear removed clients.
  * [MESOS-9786](https://issues.apache.org/jira/browse/MESOS-9786) - Race between two `REMOVE_QUOTA` calls crashes the master.
  * [MESOS-9803](https://issues.apache.org/jira/browse/MESOS-9803) - Memory leak caused by an infinite chain of futures in `UriDiskProfileAdaptor`.
  * [MESOS-9831](https://issues.apache.org/jira/browse/MESOS-9831) - Master should not report disconnected resource providers.
  * [MESOS-9852](https://issues.apache.org/jira/browse/MESOS-9852) - Slow memory growth in master due to deferred deletion of offer filters and timers.
  * [MESOS-9856](https://issues.apache.org/jira/browse/MESOS-9856) - REVIVE call with specified role(s) clears filters for all roles of a framework.
  * [MESOS-9870](https://issues.apache.org/jira/browse/MESOS-9870) - Simultaneous adding/removal of a role from framework's roles and its suppressed roles crashes the master.
  * [MESOS-9695](https://issues.apache.org/jira/browse/MESOS-9695) - Remove the duplicate pid check in Docker containerizer
  * [MESOS-9759](https://issues.apache.org/jira/browse/MESOS-9759) - Log required quota headroom and available quota headroom in the allocator.
  * [MESOS-9787](https://issues.apache.org/jira/browse/MESOS-9787) - Log slow SSL (TLS) peer reverse DNS lookup.


Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.8.1)

### Upgrades

Rolling upgrades from a Mesos 1.8.0 cluster to Mesos 1.8.1 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.8.1 from 1.7.x, 1.6.x, or 1.5.x.

### Try it out

Please try out this release and let us know what you think. If you run into any issues, let us know on the [user mailing list and/or Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 9 contributors who made 1.8.1 possible:

Andrei Budnik, Andrei Sekretenko, Benjamin Mahler, Chun-Hung Hsiao, Gilbert Song, Joseph Wu, Meng Zhu, Qian Zhang, Stéphane Cottin
