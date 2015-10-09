---
layout: post
title: Apache Mesos 0.14.2 Released
permalink: /blog/mesos-0-14-2-released/
published: true
post_author:
  display_name: Ben Mahler
  gravatar: fb43656d4d45f940160c3226c53309f5
  twitter: bmahler
tags: Release
---

We recently released Mesos 0.14.2, a bugfix release with only a minor change related to cgroups isolation in 0.14.1. If you're using 0.14.1 with cgroups isolation, it is recommended to upgrade to avoid unnecessary out-of-memory (OOM) killing of jobs. The latest version of Mesos is available on our [downloads](http://mesos.apache.org/downloads/) page.

### Upgrading
If upgrading from 0.14.x, this upgrade can be applied seamlessly to running clusters. However, if you're upgrading from 0.13.x on a running cluster, please refer to the [Upgrades](http://mesos.apache.org/documentation/latest/upgrades/) document, which details how a seamless upgrade from 0.13.x to 0.14.x can be performed.

### Changes since 0.14.1
With the release of 0.14.1, when using cgroups isolation, the OOM semantics were altered to enable the kernel OOM killer and to use the memory soft limit combined with memory threshold notifications to induce OOMs in user-space. This was done to attempt to capture memory statistics at the time of OOM for diagnostic purposes. However, this proved to trigger unintended OOMs as the memory purging that occurs when the hard limit is reached was no longer occurring.

0.14.2 no longer uses threshold notifications; the memory hard limit is now used instead to preserve the previous OOM semantics.

For additional details, please see:
[MESOS-755](https://issues.apache.org/jira/browse/MESOS-755), 
[MESOS-762](https://issues.apache.org/jira/browse/MESOS-762).