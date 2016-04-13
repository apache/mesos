---
layout: post
title: Apache Mesos 0.26.1 and More Released
permalink: /blog/mesos-0-26-1-and-more-released/
published: true
post_author:
  display_name: Michael Park
  gravatar: 2ab9cab3a7cf782261c583c1f48a81b0
  twitter: mcypark
tags: Release
---

The latest Mesos 0.26.1, 0.25.1 and 0.24.2 are now available for [download](/downloads).
The applicable subset of the following issues were backported.

* [MESOS-920](https://issues.apache.org/jira/browse/MESOS-920) - GLOG performance.
* [MESOS-3046](https://issues.apache.org/jira/browse/MESOS-3046) - UUID performance.
* [MESOS-3051](https://issues.apache.org/jira/browse/MESOS-3051) - Port Ranges performance.
* [MESOS-3052](https://issues.apache.org/jira/browse/MESOS-3052) - Allocator filter performance.
* [MESOS-3307](https://issues.apache.org/jira/browse/MESOS-3307) - Configurable task/framework history.
* [MESOS-3560](https://issues.apache.org/jira/browse/MESOS-3560) - JSON-based credential files.
* [MESOS-3602](https://issues.apache.org/jira/browse/MESOS-3602) - HDFS.
* [MESOS-3738](https://issues.apache.org/jira/browse/MESOS-3738) - Mesos health check within docker container.
* [MESOS-3773](https://issues.apache.org/jira/browse/MESOS-3773), [MESOS-4069](https://issues.apache.org/jira/browse/MESOS-4069) - SSL.
* [MESOS-3834](https://issues.apache.org/jira/browse/MESOS-3834) - Agent upgrade compatibility
* [MESOS-4106](https://issues.apache.org/jira/browse/MESOS-4106) - Health checks.
* [MESOS-4235](https://issues.apache.org/jira/browse/MESOS-4235) - `/state` endpoint performance.
* [MESOS-4302](https://issues.apache.org/jira/browse/MESOS-4302) - Offer filter timeout fix for backlogged allocator.
* [MESOS-4687](https://issues.apache.org/jira/browse/MESOS-4687) - Fixed point resources math.
* [MESOS-4711](https://issues.apache.org/jira/browse/MESOS-4711) - Libevent.
* [MESOS-4979](https://issues.apache.org/jira/browse/MESOS-4979) - Deletion of special files.
* [MESOS-5021](https://issues.apache.org/jira/browse/MESOS-5021) - Memory leak in subprocess.

Full release notes are available in the release [CHANGELOG](https://github.com/apache/mesos/blob/master/CHANGELOG).

Upgrading to 0.26.1 can be done seamlessly on a 0.26.0 cluster. If upgrading from an earlier version, please refer to the [upgrades](http://mesos.apache.org/documentation/latest/upgrades/) documentation.

## Contributors

Thanks to the 20 contributors who made 0.26.1, 0.25.1, and 0.24.2 possible:

Alexander Rojas, Alexander Rukletsov, Anand Mazumdar, Benjamin Mahler, Felix Abecassis, Gilbert Song, haosdent huang, Isabel Jimenez, James Peach, Joerg Schad, Jojy Varghese, Joris Van Remoortere, Joseph Wu, Kapil Arya, Kevin Klues, Klaus Ma, Michael Park, Neil Conway, Till Toenshoff, Timothy Chen.
