---
layout: post
title: Apache Mesos 0.24.0 Released
permalink: /blog/mesos-0-24-0-released/
published: true
post_author:
  display_name: Vinod Kone
tags: Release
---

The latest Mesos release, 0.24.0, is now available for [download](http://mesos.apache.org/downloads). This release includes the following features and improvements:

#### Scheduler HTTP API ([MESOS-2288](https://issues.apache.org/jira/browse/MESOS-2288))

Mesos 0.24 provides **experimental** support for scheduler HTTP API. Framework schedulers can communicate with Mesos by sending HTTP POST requests to `/api/v1/scheduler` instead of depending on the native libmesos library. The endpoint accepts both JSON and Protobuf requests. Additionally, the master talks back to the scheduler using the same connection opened by the scheduler, mitigating communication issues in firewalled/NATed environments. Refer to the [scheduler http api documentation](http://mesos.apache.org/documentation/latest/scheduler_http_api/) for more information.

Note that, this release only adds the support for scheduler HTTP API. Support for HTTP executor API is currently in the works and will be released soon!

### API Versioning ([MESOS-3167](https://issues.apache.org/jira/browse/MESOS-3167))

As part of this release, we have also outlined the versioning scheme for Mesos HTTP API going forward. At a high level, the Mesos API (constituting Scheduler, Executor, Internal, Operator/Admin APIs) will have a version in the URL. The versioned URL will have a prefix of `/api/vN` where `N` is the version of the API. For simplicity, the stable version of the API will correspond to the major release version of Mesos. For example, v1 of the API will be supported by Mesos release versions 1.0.0, 1.4.0, 1.20.0 etc. Refer to the [versioning design document](https://docs.google.com/document/d/1-iQjo6778H_fU_1Zi_Yk6szg8qj-wqYgVgnx7u3h6OU/edit#) for more information.


### Changelog
Hundreds of other bug fixes/improvements are included in Mesos 0.24.0.
See the [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=0.24.0) for a full list of resolved JIRA issues.

### Upgrades

Rolling upgrades from a Mesos 0.23.x cluster to Mesos 0.24 are straightforward, but there are a few caveats/deprecations.
Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 0.24.


### Try it out

We encourage you to try out this release and let us know what you think. If you run into any issues, please let us know on the [user mailing list and IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 37 contributors who made 0.24.0 possible:

Adam B, Aditi Dixit, Alex Clemmer, Alexander Rojas, Alexander Rukletsov, Anand Mazumdar, Anindya Sinha, Artem Harutyunyan, Bartek Plotka, Benjamin Hindman, Benjamin Mahler, Bernd Mathiske, Chi Zhang, Chris Heller, Dave Lester, Greg Mann, Guangya Liu, Ian Downes, Isabel Jimenez, James DeFelice, James Peach, Jan Schlicht, Jiang Yan Xu, Jie Yu, Joerg Schad, Jojy Varghese, Joris Van Remoortere, Joseph Wu, Kapil Arya, Klaus Ma, Lily Chen, Marco Massenzio, Mark Wang, Michael Park, Michael Schenck, Niklas Nielsen, Paul Brett, Ryuichi Okumura, Shuai Lin, Till Toenshoff, Tim Anderegg, Timothy Chen, Vinod Kone, Yong Qiao Wang, ayouwei, haosdent huang, usultrared.