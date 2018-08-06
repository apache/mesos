---
layout: post
title: Apache Mesos 1.0.1 Released
permalink: /blog/mesos-1-0-1-released/
published: true
post_author:
  display_name: Vinod Kone
  twitter: vinodkone
tags: Release
---

The latest Mesos release, 1.0.1, is now available for [download](http://mesos.apache.org/downloads). This release includes some important bug fixes and improvements on top of 1.0.0. It is highly recommended to use this version if you are considering using Mesos 1.0. More specifically, this release includes the following fixes and improvements:

[MESOS-5388]( https://issues.apache.org/jira/browse/MESOS-5388) - MesosContainerizerLaunch flags execute arbitrary commands via shell.

[MESOS-5862]( https://issues.apache.org/jira/browse/MESOS-5862) - External links to .md files broken.

[MESOS-5911]( https://issues.apache.org/jira/browse/MESOS-5911) - Webui redirection to leader in browser does not work

[MESOS-5913]( https://issues.apache.org/jira/browse/MESOS-5913) - Stale socket FD usage when using libevent + SSL.

[MESOS-5922]( https://issues.apache.org/jira/browse/MESOS-5922) - mesos-agent --help exit status is 1

[MESOS-5923]( https://issues.apache.org/jira/browse/MESOS-5923) - Ubuntu 14.04 LTS GPU Isolator "/run" directory is noexec

[MESOS-5927]( https://issues.apache.org/jira/browse/MESOS-5927) - Unable to run "scratch" Dockerfiles with Unified Containerizer.

[MESOS-5928]( https://issues.apache.org/jira/browse/MESOS-5928) - Agent's '--version' flag doesn't work

[MESOS-5930]( https://issues.apache.org/jira/browse/MESOS-5930) - Orphan tasks can show up as running after they have finished.

[MESOS-5943]( https://issues.apache.org/jira/browse/MESOS-5943) - Incremental http parsing of URLs leads to decoder error

[MESOS-5945]( https://issues.apache.org/jira/browse/MESOS-5945) - NvidiaVolume::create() should check for root before creating volume

[MESOS-5959]( https://issues.apache.org/jira/browse/MESOS-5959) - All non-root tests fail on GPU machine

[MESOS-5969]( https://issues.apache.org/jira/browse/MESOS-5969) - Linux 'MountInfoTable' entries not sorted as expected

[MESOS-5982]( https://issues.apache.org/jira/browse/MESOS-5982) - NvidiaVolume errors out if any binary is missing

[MESOS-5986]( https://issues.apache.org/jira/browse/MESOS-5986) - SSL Socket CHECK can fail after socket receives EOF

[MESOS-5988]( https://issues.apache.org/jira/browse/MESOS-5988) - PollSocketImpl can write to a stale fd.

[MESOS-5830]( https://issues.apache.org/jira/browse/MESOS-5830) - Make a sweep to trim excess space around angle brackets

[MESOS-5970]( https://issues.apache.org/jira/browse/MESOS-5970) - Remove HTTP_PARSER_VERSION_MAJOR < 2 code in decoder.

Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.0.1)

### Upgrades

Rolling upgrades from a Mesos 1.0.0 cluster to Mesos 1.0.1 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.0.1.

### Try it out

We encourage you to try out this release and let us know what you think. If you run into any issues, please let us know on the [user mailing ld Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 12 contributors who made 1.0.1 possible

Anand Mazumdar, Benjamin Mahler, Gilbert Song, Greg Mann, Jiang Yan Xu, Jie Yu, Joris Van Remoortere, Kevin Klues, Pierre Cheynier, Vinod Kone, haosdent huang and zhou xing.
