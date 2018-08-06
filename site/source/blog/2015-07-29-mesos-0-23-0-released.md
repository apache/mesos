---
layout: post
title: Apache Mesos 0.23.0 Released
permalink: /blog/mesos-0-23-0-released/
published: true
post_author:
  display_name: Adam B
tags: Release
---

The latest Mesos release, 0.23.0, is now available for [download](http://mesos.apache.org/downloads). This release includes the following features and improvements:

#### Per-container network isolation ([MESOS-1585](https://issues.apache.org/jira/browse/MESOS-1585))
Mesos 0.23 provides support for per-container network monitoring and isolation. Network isolation prevents a single container from exhausting the available network ports, consuming an unfair share of the network bandwidth or significantly delaying packet transmission for others. Network statistics for each active container are published through the /monitor/statistics.json endpoint on the slave. Network isolation is transparent for the majority of tasks running on a slave (those that bind to port 0 and let the kernel allocate their port). This feature is only available on Linux and requires a configure-time flag.
Refer to the [network monitoring and isolation documentation](http://mesos.apache.org/documentation/latest/network-monitoring/) for more information.


#### SSL ([MESOS-910](https://issues.apache.org/jira/browse/MESOS-910))
Experimental support for SSL encryption of any libprocess communication via libevent. Encrypting traffic between the Mesos master and its slaves and frameworks is important for information security, as it prevents eavesdropping and impersonation. This feature requires a configure-time flag and will have some performance impact.
Refer to the [Mesos SSL documentation](http://mesos.apache.org/documentation/latest/mesos-ssl/) for instructions on building and enabling SSL.


#### Oversubscription ([MESOS-354](https://issues.apache.org/jira/browse/MESOS-354))
Experimental support for launching tasks/executors on revocable resources.  These resources can be revoked by Mesos at any time, causing the tasks using them to be throttled or preempted.

High-priority user-facing services are typically provisioned on large clusters for peak load and unexpected load spikes. Hence, for most of time, the provisioned resources remain underutilized. Oversubscription takes advantage of temporarily unused resources to execute best-effort tasks such as background analytics, video/image processing, chip simulations, and other low priority jobs.

Oversubscription adds two new slave components: a Resource Estimator and a Quality of Service (QoS) Controller, alongside extending the existing resource allocator, resource monitor, and mesos slave.
Refer to the [oversubscription documentation](http://mesos.apache.org/documentation/latest/oversubscription/) for more information.


#### Persistent volumes ([MESOS-1554](https://issues.apache.org/jira/browse/MESOS-1554))
Experimental support for frameworks creating Persistent Volumes from disk resources.  This enables stateful services such as HDFS and Cassandra to store their data within Mesos rather than having to resort to network-mounted EBS volumes or unmanaged disk resources that need to be placed in a well-known location.
Refer to the [persistent volume documentation](http://mesos.apache.org/documentation/latest/persistent-volume/) for more information.

#### Dynamic reservations ([MESOS-2018](https://issues.apache.org/jira/browse/MESOS-2018))
Experimental support for frameworks dynamically reserving resources on specific slaves for their role.  Rather than requiring an operator to specify a fixed, precalculated set of "static" reservations on slave startup, a framework can now reserve resources as they are being offered, without requiring a slave restart.
No breaking changes were introduced with dynamic reservation, which means the existing static reservation mechanism continues to be fully supported.
Refer to the [reservation documentation](http://mesos.apache.org/documentation/latest/reservation/) for more information.

#### Fetcher caching ([MESOS-336](https://issues.apache.org/jira/browse/MESOS-336))
Experimental support for fetcher caching of executor/task binaries. The fetcher can be instructed to cache URI downloads in a dedicated directory for reuse by subsequent downloads.  If the URI’s “cache” field has the value “true”, then the fetcher cache is in effect. If a URI is encountered for the first time (for a particular user), it is first downloaded into the cache, then copied to the sandbox directory from there. If the same URI is encountered again (for the same user), and a corresponding cache file is resident in the cache or still en route into the cache, then downloading is omitted and the fetcher proceeds directly to copying from the cache.
Refer to the [fetcher documentation](http://mesos.apache.org/documentation/latest/fetcher/) for more information.

#### NOTE: Experimental status
SSL, Oversubscription, Persistent Volumes, Dynamic Reservations, and Fetcher Caching are all released as "experimental" features, meaning that they are feature complete, at least for some use case, but have not yet been tested in production environments. We welcome your feedback.

### Changelog
Hundreds of other bug fixes/improvements are included in Mesos 0.23.0.
See the [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=0.23.0) for a full list of resolved JIRA issues.

### Upgrades

Rolling upgrades from a Mesos 0.22.x cluster to Mesos 0.23 are straightforward, but there are a few caveats/deprecations.
Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 0.23.

### Compiler version requirement

Please note that Mesos 0.23.0 now requires gcc 4.8+ or clang 3.5+, so that we can take advantage of C++11 language features.

### Try it out

We encourage you to try out this release and let us know what you think. If you run into any issues, please let us know on the [user mailing list and IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 52 contributors who made 0.23.0 possible:
Aaron Bell, Adam B, Aditi Dixit, Akanksha Agrawal, Alexander Rojas, Alexander Rukletsov, Anand Mazumdar, Artem Harutyunyan, Bartek Plotka, Benjamin Hindman, Benjamin Mahler, Bernd Mathiske, Brendan Chang, Brian Wickman, Chi Zhang, Christos Kozyrakis, Cody Maloney, Cong Wang, Connor Doyle, Dave Lester, Dominic Hamon, Evelina Dumitrescu, Gajewski, Greg Mann, haosdent huang, Ian Babrou, Ian Downes, Isabel Jimenez, Itamar Ostricher, James Peach, Jan Schlicht, Jay Buffington, Jiang Yan Xu, Jie Yu, Joerg Schad, Jojy Varghese, Joris Van Remoortere, Kapil Arya, Marco Massenzio, Mark Wang, Michael Park, Nancy  Ko, Niklas Q. Nielsen, Oliver Nicholas, Paul Brett, Ricardo Cervera-Navarro, Stan Teresen, Till Toenshoff, Timothy Chen, Vinod Kone, weitao zhou, and Zhiwei Chen
