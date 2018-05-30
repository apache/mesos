---
layout: post
title: "Mesos 1.6: Persistent Volume, Scheduler API, and Docker Support Improvements"
published: true
post_author:
  display_name: Greg Mann
  gravatar: 20c5c9695f0891b6b093704331238133
  twitter: greggomann
tags: Storage, Docker, Performance
---

Great work everyone; Apache Mesos 1.6.0 was released on May 11!

# New Features and Improvements

Mesos 1.6 includes improvements to persistent volumes, usability of the scheduler API, Docker daemon support, and memory allocation performance.

## Resizing Persistent Volumes

Mesos 1.6 adds experimental support for the [resizing of persistent volumes](http://mesos.apache.org/documentation/latest/persistent-volume/). Two new operations, [GrowVolume](http://mesos.apache.org/documentation/latest/persistent-volume/#offer-operation-growvolume) and [ShrinkVolume](http://mesos.apache.org/documentation/latest/persistent-volume/#offer-operation-shrinkvolume), can be used both via the [scheduler API](http://mesos.apache.org/documentation/latest/scheduler-http-api/#accept) and via the [operator API](http://mesos.apache.org/documentation/latest/operator-http-api/#grow_volume).

Persistent volume resizing is currently limited to volumes created on default agent disk resources - in other words, CSI volumes are not yet supported.

## Offer Operation Feedback

Mesos 1.6 introduces experimental support for offer operation status updates. Previously, only `Launch` and `LaunchGroup` operations received feedback via task status updates. Now, schedulers may opt to receive feedback on other offer operations via operation status updates by setting the `id` field in the operation. Similar to task status updates, when these operation updates have a unique `uuid` set, the scheduler must acknowledge them. This feature allows schedulers to ensure the reliability of offer operations like `Reserve` and `Create`; if such an operation fails and the scheduler has opted in to receive feedback, an operation status update will be sent and the scheduler can retry if appropriate.

Offer operation feedback is currently limited to operations on resources provided by a resource provider - in most cases, this means that only operations on CSI volumes are supported.

## Performance and Debugging

Mesos 1.6 includes experimental support for the [jemalloc](http://jemalloc.net) memory allocation library. In addition to a potential improvement in memory allocation performance, this enables simple memory profiling via command-line flags and environment variables outlined in the [documentation](http://mesos.apache.org/documentation/latest/memory-profiling/). We expect this feature to help drive future performance improvments in Mesos since it improves the ability of operators to understand how Mesos behaves in production environments.

## Docker Improvements

In response to issues encountered by operators when using Mesos with the Docker container runtime, [several improvements](https://issues.apache.org/jira/browse/MESOS-8572) were made to the Docker containerizer and executor to help them more gracefully handle situations in which the Docker daemon is unresponsive. Retry and timeout logic has been added in several key locations so that when the Docker daemon hangs for short periods of time, Mesos can recover and resume normal operation.

## Upgrade

In most cases, upgrades from Mesos 1.5.0 to Mesos 1.6.0 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.6.0.

Support for the Container Storage Interface (CSI) version 0.2 has been added, and CSI v0.1 support is now deprecated.

When building Mesos with gRPC support, note that gRPC version 1.10 or later is now required.

# Community

Inspired by the work that went into this release? Want to get involved? Have feedback? We'd love to hear from you! Join a [working group](http://mesos.apache.org/community/#working-groups) or start a conversation in the [community](http://mesos.apache.org/community/)!

# Thank you!

Thanks to the 55 contributors who made Mesos 1.6.0 possible:

Adam B, Akash Gupta, Alexander Rojas, Alexander Rukletsov, Andrei Budnik, Andrew Schwartzmeyer, AntonDan, Armand Grillet, Benjamin Bannier, Benjamin Mahler, Benjamin Peterson, Benno Evers, Chun-Hung Hsiao, Clement Michaud, David Forsythe, Demitri Swan, EMumau, Eric Chung, Eric Mumau, Gaston Kleiman, Gilbert Song, Greg Mann, Harold Dost, Ilya Pronin, James Peach, Jan Schlicht, Jason Lai, Jeff Coffler, Jiang Yan Xu, Jie Yu, Joerg Schad, John Kordich, Joseph Wu, Judith Malnick, Kapil Arya, Marcin Owsiany, Megha Sharma, Meng Zhu, Michael Park, Murilo Pereira, Packt, Qian Zhang, Sachin Paryani, Sagar Patwardhan, Senthil Kumaran, Shiv Deepak, Shubham Kumar, Sofian Brabez, Till Toenshoff, Tomasz Janiszewski, Vinod Kone, Xudong Ni, Zhitao Li, longfei, miamipanther

Enjoy Apache Mesos 1.6!
