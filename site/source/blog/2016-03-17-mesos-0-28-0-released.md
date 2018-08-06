---
layout: post
title: Apache Mesos 0.28.0 Released
permalink: /blog/mesos-0-28-0-released/
published: true
post_author:
  display_name: Vinod Kone
  twitter: vinodkone
tags: Release
---

The latest Mesos release, 0.28.0, is now available for [download](http://mesos.apache.org/downloads).

This release includes the following features and improvements:

  * [MESOS-4343] - A new cgroups isolator for enabling the net_cls subsystem in
    Linux. The cgroups/net_cls isolator allows operators to provide network
    performance isolation and network segmentation for containers within a Mesos
    cluster. To enable the cgroups/net_cls isolator, append `cgroups/net_cls` to
    the `--isolation` flag when starting the slave. Please refer to
    docs/mesos-containerizer.md for more details.

  * [MESOS-4687] - The implementation of scalar resource values (e.g., "2.5
    CPUs") has changed. Mesos now reliably supports resources with up to three
    decimal digits of precision (e.g., "2.501 CPUs"); resources with more than
    three decimal digits of precision will be rounded. Internally, resource math
    is now done using a fixed-point format that supports three decimal digits of
    precision, and then converted to/from floating point for input and output,
    respectively. Frameworks that do their own resource math and manipulate
    fractional resources may observe differences in roundoff error and numerical
    precision.

  * [MESOS-4479] - Reserved resources can now optionally include "labels".
    Labels are a set of key-value pairs that can be used to associate metadata
    with a reserved resource. For example, frameworks can use this feature to
    distinguish between two reservations for the same role at the same agent
    that are intended for different purposes.

  * [MESOS-2840] - **Experimental** support for container images in Mesos
    containerizer (a.k.a. Unified Containerizer). This allows frameworks to
    launch Docker/Appc containers using Mesos containerizer without relying on
    docker daemon (engine) or rkt. The isolation of the containers is done using
    isolators. Please refer to docs/container-image.md for currently supported
    features and limitations.

  * [MESOS-4793] - **Experimental** support for v1 Executor HTTP API. This
    allows executors to send HTTP requests to the /api/v1/executor agent
    endpoint without the need for an executor driver. Please refer to
    docs/executor-http-api.md for more details.

  * [MESOS-4370] Added support for service discovery of Docker containers that
    use Docker Remote API v1.21.

Additional API Changes:

  * [MESOS-4066] - Agent should not return partial state when a request is made to /state endpoint during recovery.
  * [MESOS-4547] - Introduce TASK_KILLING state.
  * [MESOS-4712] - Remove 'force' field from the Subscribe Call in v1 Scheduler API.
  * [MESOS-4591] - Change the object of ReserveResources and CreateVolume ACLs to `roles`.
  * [MESOS-3583] - Add stream IDs for HTTP schedulers.
  * [MESOS-4427] - Ensure ip_address in state.json (from NetworkInfo) is valid


Furthermore, several bugfixes and improvements made it into this release.
For full release notes with all features and bug fixes, please refer to the [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=0.28.0).

### Upgrades

Rolling upgrades from a Mesos 0.27.0 cluster to Mesos 0.28.0 are straightforward. There are just some minor, backwards compatible deprecations.
Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 0.28.0.


### Try it out

We encourage you to try out this release and let us know what you think.
If you run into any issues, please let us know on the [user mailing list and IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 44 contributors who made 0.28.0 possible:

Abhishek Dasgupta,Alex Clemmer,Alex Naparu,Alexander Rojas,Alexander Rukletsov,Anand Mazumdar,Avinash sridharan,Benjamin Bannier,Benjamin Mahler,Bernd Mathiske,Cong Wang,Daniel Pravat,David Forsythe,Diana Arroyo,Disha  Singh,Gilbert Song,Greg Mann,Guangya Liu,Isabel Jimenez,James Peach,Jan Schlicht,Jie Yu,Joerg Schad,Jojy Varghese,Joris Van Remoortere,Joseph Wu,Kapil Arya,Kevin Devroede,Kevin Klues,Klaus Ma,M Lawindi,Michael Browning,Michael Lunøe,Michael Park,Neil Conway,Shuai Lin,Till Toenshoff,Timothy Chen,Vinod Kone,Yong Tang,Yongqiao Wang,Zhiwei Chen,haosdent huang,mlawindi
