---
layout: post
title: Apache Mesos 1.3.1 Released
permalink: /blog/mesos-1-3-1-released/
published: true
post_author:
  display_name: Michael Park
  gravatar: 2ab9cab3a7cf782261c583c1f48a81b0
  twitter: mcypark
tags: Release
---

The latest Mesos 1.3.x release, 1.3.1, is now available for [download](http://mesos.apache.org/downloads). This release includes important bug fixes and improvements on top of 1.3.0. It is recommended to use this version if you are considering using Mesos 1.3. More specifically, this release includes the following:

* [MESOS-5187](https://issues.apache.org/jira/browse/MESOS-5187) - The filesystem/linux isolator does not set the permissions of the host_path.
* [MESOS-7252](https://issues.apache.org/jira/browse/MESOS-7252) - Need to fix resource check in long-lived framework.
* [MESOS-7429](https://issues.apache.org/jira/browse/MESOS-7429) - Allow isolators to inject task-specific environment variables.
* [MESOS-7540](https://issues.apache.org/jira/browse/MESOS-7540) - Add an agent flag for executor re-registration timeout.
* [MESOS-7546](https://issues.apache.org/jira/browse/MESOS-7546) - `WAIT_NESTED_CONTAINER` sometimes returns 404.
* [MESOS-7569](https://issues.apache.org/jira/browse/MESOS-7569) - Allow "old" executors with half-open connections to be preserved during agent upgrade / restart.
* [MESOS-7581](https://issues.apache.org/jira/browse/MESOS-7581) - Fix interference of external Boost installations when using some unbundled dependencies.
* [MESOS-7689](https://issues.apache.org/jira/browse/MESOS-7689) - Libprocess can crash on malformed request paths for libprocess messages.
* [MESOS-7690](https://issues.apache.org/jira/browse/MESOS-7690) - The agent can crash when an unknown executor tries to register.
* [MESOS-7692](https://issues.apache.org/jira/browse/MESOS-7692) - Default environment variables defined in Docker image are not available in Mesos containerizer.
* [MESOS-7703](https://issues.apache.org/jira/browse/MESOS-7703) - Mesos fails to exec a custom executor when no shell is used.
* [MESOS-7728](https://issues.apache.org/jira/browse/MESOS-7728) - Java HTTP adapter crashes JVM when leading master disconnects.
* [MESOS-7770](https://issues.apache.org/jira/browse/MESOS-7770) - Persistent volume might not be mounted if there is a sandbox volume whose source is the same as the target of the persistent volume.
* [MESOS-7777](https://issues.apache.org/jira/browse/MESOS-7777) - Agent failed to recover due to mount namespace leakage in Docker 1.12/1.13.
* [MESOS-7796](https://issues.apache.org/jira/browse/MESOS-7796) - `LIBPROCESS_IP` isn't passed on to the fetcher.
* [MESOS-7830](https://issues.apache.org/jira/browse/MESOS-7830) - Sandbox_path volume does not have ownership set correctly.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.3.1)

### Upgrades

Rolling upgrades from a Mesos 1.3.0 cluster to Mesos 1.3.1 are straightforward.
Please refer to the [upgrade
guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.3.1 from 1.2.x, 1.1.x or 1.0.x.

### Try it out

Please try out this release and let us know what you think. If you run into any issues, let us know on the [user mailing list and/or Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 52 contributors who made 1.3.1 possible:

Aaron Wood, Adam B, Alexander Rojas, Alexander Rukletsov, Anand Mazumdar, Andrei Budnik, Andrew Schwartzmeyer, Anindya Sinha, Armand Grillet, Avinash sridharan, Benjamin Bannier, Benjamin Hindman, Benjamin Mahler, Benno Evers, Chun-Hung Hsiao, Conrad Kleinespel, Dmitry Zhuk, Eric Chung, Gastón Kleiman, Gilbert Song, Greg Mann, Haosdent Huang, Jacob Janco, James Peach, Jan Schlicht, Jay Guo, Jeff Coffler, Jiang Yan Xu, Jie Yu, John Kordich, Joseph Wu, Kapil Arya, Kevin Klues, Li Li, Mao Geng, Megha Sharma, Michael Park, Neil Conway, Olivier Sallou, Qian Zhang, Quinn Leng, Shingo Kitayama, Silas Snider, Till Toenshoff, Tim Hansen, TobiLG, Vinod Kone, Zhitao Li, Zhongbo Tian, haosdent huang, hardy.jung, huangwenpeng
