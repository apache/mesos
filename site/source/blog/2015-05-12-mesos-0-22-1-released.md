---
layout: post
title: Apache Mesos 0.22.1 Released
permalink: /blog/mesos-0-22-1-released/
published: true
post_author:
  display_name: Adam B
tags: Release
---

The latest Mesos release, 0.22.1 is now available for [download](http://mesos.apache.org/downloads). This release includes fixes to containerizer recovery, sandbox permissions, and the Java and Python bindings. Also, certain fields of FrameworkInfo are now updated on reregistration.

* [MESOS-1795](https://issues.apache.org/jira/browse/MESOS-1795) - Assertion failure in state abstraction crashes JVM.
* [MESOS-2161](https://issues.apache.org/jira/browse/MESOS-2161) - AbstractState JNI check fails for Marathon framework.
* [MESOS-2461](https://issues.apache.org/jira/browse/MESOS-2461) - Slave should provide details on processes running in its cgroups
* [MESOS-2583](https://issues.apache.org/jira/browse/MESOS-2583) - Tasks getting stuck in staging.
* [MESOS-2592](https://issues.apache.org/jira/browse/MESOS-2592) - The sandbox directory is not chown'ed if the fetcher doesn't run.
* [MESOS-2601](https://issues.apache.org/jira/browse/MESOS-2601) - Tasks are not removed after recovery from slave and mesos containerizer
* [MESOS-2614](https://issues.apache.org/jira/browse/MESOS-2614) - Update name, hostname, failover_timeout, and webui_url in master on framework re-registration
* [MESOS-2643](https://issues.apache.org/jira/browse/MESOS-2643) - Python scheduler driver disables implicit acknowledgments by default.
* [MESOS-2668](https://issues.apache.org/jira/browse/MESOS-2668) - Slave fails to recover when there are still processes left in its cgroup

Full release notes are available in the release [CHANGELOG](https://github.com/apache/mesos/blob/master/CHANGELOG).

Upgrading to 0.22.1 can be done seamlessly on a 0.22.0 cluster. If upgrading from an earlier version, please refer to the [upgrades](http://mesos.apache.org/documentation/latest/upgrades/) documentation.

## Contributors
Thanks to all the contributors for 0.22.1: Benjamin Hindman, Benjamin Mahler, Ian Downes, Jie Yu, Joris Van Remoortere, Niklas Nielsen, and Timothy Chen.