---
layout: post
title: Apache Mesos 0.20.1 Released
permalink: /blog/mesos-0-20-1-released/
published: true
post_author:
  display_name: Adam B
tags: Release
---

The latest Mesos release, 0.20.1 is now available for [download](http://mesos.apache.org/downloads). One of the major features of Mesos 0.20.0 was native Docker support, and that support has been improved in 0.20.1 by the following bug fixes and improvements:

 * [MESOS-1621](https://issues.apache.org/jira/browse/MESOS-1621) - Docker run networking should be configurable and support bridge network
 * [MESOS-1724](https://issues.apache.org/jira/browse/MESOS-1724) - Can't include port in DockerInfo's image
 * [MESOS-1730](https://issues.apache.org/jira/browse/MESOS-1730) - Should be an error if commandinfo shell=true when using docker containerizer
 * [MESOS-1732](https://issues.apache.org/jira/browse/MESOS-1732) - Mesos containerizer doesn't reject tasks with container info set
 * [MESOS-1737](https://issues.apache.org/jira/browse/MESOS-1737) - Isolation=external result in core dump on 0.20.0
 * [MESOS-1755](https://issues.apache.org/jira/browse/MESOS-1755) - Add docker support to mesos-execute
 * [MESOS-1758](https://issues.apache.org/jira/browse/MESOS-1758) - Freezer failure leads to lost task during container destruction.
 * [MESOS-1762](https://issues.apache.org/jira/browse/MESOS-1762) - Avoid docker pull on each container run
 * [MESOS-1770](https://issues.apache.org/jira/browse/MESOS-1770) - Docker with command shell=true should override entrypoint
 * [MESOS-1809](https://issues.apache.org/jira/browse/MESOS-1809) - Modify docker pull to use docker inspect after a successful pull

Full release notes are available in the release [CHANGELOG](https://github.com/apache/mesos/blob/master/CHANGELOG).

Upgrading to 0.20.1 can be done seamlessly on a 0.20.0 cluster. If upgrading from 0.19.x, please refer to the [upgrades](http://mesos.apache.org/documentation/latest/upgrades/) documentation.

## Contributors
Thanks to all the contributors for 0.20.1: Timothy Chen, Jie Yu, Timothy St. Clair, Vinod Kone, Chris Heller, Kamil Domański, and Till Toenshoff.
