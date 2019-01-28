---
layout: post
title: "Apache Mesos 1.7.1 Released"
published: true
post_author:
  display_name: Chun-Hung Hsiao & Gastón Kleiman
  gravatar: 9447267537939b034faa02cbe08fef20
  twitter: chhsia0
tags: Release
---

Apache Mesos 1.7.1 is now available for [download](http://mesos.apache.org/downloads). As we continue to focus on performance, the community can get access to the latest performance improvements in this release. The [experimental support for Container Storage Interface (CSI)](http://mesos.apache.org/documentation/latest/csi/) has been improved in this release as well. Last but not least, this release includes 38 bug fixes to 1.7.0. If you are considering using Mesos 1.7, it is recommended to use 1.7.1.

Specifically, the following critical bugs are resolved in this release:

* [MESOS-9131](https://issues.apache.org/jira/browse/MESOS-9131) - Health checks launching nested containers while a container is being destroyed lead to unkillable tasks.
* [MESOS-9228](https://issues.apache.org/jira/browse/MESOS-9228) - SLRP does not clean up plugin containers after it is removed.
* [MESOS-9279](https://issues.apache.org/jira/browse/MESOS-9279) - Docker Containerizer 'usage' call might be expensive if mount table is big.
* [MESOS-9281](https://issues.apache.org/jira/browse/MESOS-9281) - SLRP gets a stale checkpoint after system crash.
* [MESOS-9283](https://issues.apache.org/jira/browse/MESOS-9283) - Docker containerizer actor can get backlogged with large number of containers.
* [MESOS-9308](https://issues.apache.org/jira/browse/MESOS-9308) - URI disk profile adaptor could deadlock.
* [MESOS-9317](https://issues.apache.org/jira/browse/MESOS-9317) - Some master endpoints do not handle failed authorization properly.
* [MESOS-9334](https://issues.apache.org/jira/browse/MESOS-9334) - Container stuck at ISOLATING state due to libevent poll never returns.
* [MESOS-9419](https://issues.apache.org/jira/browse/MESOS-9419) - Executor to framework message crashes master if framework has not re-registered.
* [MESOS-9474](https://issues.apache.org/jira/browse/MESOS-9474) - Master does not respect authorization result for `CREATE_DISK` and `DESTROY_DISK`.
* [MESOS-9480](https://issues.apache.org/jira/browse/MESOS-9480) - Master may skip processing authorization results for `LAUNCH_GROUP`.
* [MESOS-9501](https://issues.apache.org/jira/browse/MESOS-9501) - Mesos executor fails to terminate and gets stuck after agent host reboot.
* [MESOS-9502](https://issues.apache.org/jira/browse/MESOS-9502) - IOswitchboard cleanup could get stuck due to FD leak from a race.
* [MESOS-9508](https://issues.apache.org/jira/browse/MESOS-9508) - Official 1.7.0 tarball can't be built on Ubuntu 16.04 LTS.

And this release includes the following critical improvements:

* [MESOS-6765](https://issues.apache.org/jira/browse/MESOS-6765) - "Make the Resources wrapper ""copy-on-write"" to improve performance."
* [MESOS-9239](https://issues.apache.org/jira/browse/MESOS-9239) - Improve sorting performance in the DRF sorter.
* [MESOS-9249](https://issues.apache.org/jira/browse/MESOS-9249) - Avoid dirtying the DRF sorter when allocating resources.
* [MESOS-9275](https://issues.apache.org/jira/browse/MESOS-9275) - Allow optional `profile` to be specified in `CREATE_DISK` offer operation.
* [MESOS-9305](https://issues.apache.org/jira/browse/MESOS-9305) - Create cgoup recursively to workaround systemd deleting cgroups_root.
* [MESOS-9321](https://issues.apache.org/jira/browse/MESOS-9321) - Add an optional `vendor` field in `Resource.DiskInfo.Source`.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.7.1).

### Upgrades

Rolling upgrades from a Mesos 1.7.0 cluster to Mesos 1.7.1 are straightforward. However, if your framework is consuming the [experimental support for CSI](http://mesos.apache.org/documentation/latest/csi/), the language binding used by the framework should be updated.

For detailed information on upgrading to Mesos 1.7.1 from other versions, please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/).

### Try it out

Please try out this release and let us know what you think. If you run into any issues, let us know on the [user mailing list and/or Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 20 contributors who made 1.7.1 possible:

Alexander Rojas, Alexander Rukletsov, Andrei Budnik, Benjamin Bannier, Benjamin Mahler, Benno Evers, Chun-Hung Hsiao, Deepak Goel, Gastón Kleiman, Gilbert Song, Greg Mann, James Peach, Jie Yu, Joseph Wu, Kevin Klues, Meng Zhu, Qian Zhang, Till Toenshoff, Vinod Kone, Fei Long
