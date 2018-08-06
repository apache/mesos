---
layout: post
title: Apache Mesos 0.25.0 Released
permalink: /blog/mesos-0-25-0-released/
published: true
post_author:
  display_name: Niklas Q. Nielsen
  twitter: quarfot
tags: Release
---

The latest Mesos release, 0.25.0, is now available for [download](http://mesos.apache.org/downloads).
This release includes the following features and improvements:

 * [[MESOS-1474]](https://issues.apache.org/jira/browse/MESOS-1474) - Experimental support for maintenance primitives. Please refer to [the maintenance documentation](http://mesos.apache.org/documentation/latest/maintenance/) for more information.
 * [[MESOS-2600]](https://issues.apache.org/jira/browse/MESOS-2600) - Added master endpoints /reserve and /unreserve for dynamic reservations. Please refer to [the reservation documentation](http://mesos.apache.org/documentation/latest/reservation/) for more information.
 * [[MESOS-2044]](https://issues.apache.org/jira/browse/MESOS-2044) - Extended Module APIs to enable IP per container assignment, isolation and resolution. Please refer to [the container networking documentation](http://mesos.apache.org/documentation/latest/networking-for-mesos-managed-containers/) for more information.

Furthermore, 100+ bugs and improvements have made it into this release.
For full release notes with all features and bug fixes, please refer to the [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=0.25.0).

### Upgrades

Rolling upgrades from a Mesos 0.24.x cluster to Mesos 0.25 are straightforward, but there are a few caveats/deprecations.
Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 0.25.


### Try it out

We encourage you to try out this release and let us know what you think.
If you run into any issues, please let us know on the [user mailing list and IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 40 contributors who made 0.25.0 possible:
Adam B, Aditi Dixit, Alex Clemmer, Alexander Rukletsov, Anand Mazumdar, Artem Harutyunyan, Ben Mahler, Bernd Mathiske, Chi Zhang, Cong Wang, Eijsermans, Eren Güve, Felix Abecassis, Greg Mann, Guangya Liu, Isabel Jimenez, James Peach, Jan Schlicht, Jiang Yan Xu, Jie Yu, Joerg Schad, Jojy Varghese, Joris Van Remoortere, Joseph Wu, Kapil Arya, Klaus Ma, Lily Chen, M Bauer, Marco Massenzio, Michael Park, Neil Conway, Niklas Nielsen, Paul Brett, Qian Zhang, Timothy Chen, Vaibhav Khanduja, Vinod Kone, Wojciech Sielski, Yong Qiao Wang and Haosdent Huang
