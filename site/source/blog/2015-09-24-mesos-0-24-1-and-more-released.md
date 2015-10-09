---
layout: post
title: Apache Mesos 0.24.1 and More Released
permalink: /blog/mesos-0-24-1-and-more-released/
published: true
post_author:
  display_name: Adam B
tags: Release
---

The latest Mesos 0.24.1 is now available for [download](/downloads). This release includes a fix for version parsing for Docker 1.8 and 1.7.0.fc22, as well as a fix for Docker command health checks. These same fixes were backported onto 0.23.0 to produce a new Mesos 0.23.1 release. MESOS-2986 was also backported to 0.21 and 0.22 to produce Mesos 0.21.2 and 0.22.2 (still VOTING). All of these releases are (or will be) available on the [downloads](/downloads) page.

* [MESOS-2986](https://issues.apache.org/jira/browse/MESOS-2986) - Docker version output is not compatible with Mesos
* [MESOS-3136](https://issues.apache.org/jira/browse/MESOS-3136) - COMMAND health checks with Marathon 0.10.0 are broken

Full release notes are available in the release [CHANGELOG](https://github.com/apache/mesos/blob/master/CHANGELOG).

Upgrading to 0.24.1 can be done seamlessly on a 0.24.0 cluster. In fact, only the slaves need to be upgraded. If upgrading from an earlier version, please refer to the [upgrades](http://mesos.apache.org/documentation/latest/upgrades/) documentation.

## Contributors
Special thanks to the lone code contributor for 0.24.1, 0.23.1, 0.22.2, and 0.21.2: haosdent huang. Our hero.
