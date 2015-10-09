---
layout: post
title: Apache Mesos 0.22.0 Released
permalink: /blog/mesos-0-22-0-released/
published: true
post_author:
  display_name: Niklas Q. Nielsen
  twitter: quarfot
tags: Release
---

The latest Mesos release, 0.22.0, is now available for [download](http://mesos.apache.org/downloads). This release includes several major features:

* Disk quota isolation in Mesos containerizer; refer to the [containerization documentation](http://mesos.apache.org/documentation/latest/mesos-containerizer/) to enable disk quota monitoring and enforcement.
* Support for explicitly sending status updates acknowledgements from schedulers; refer to the [upgrades document](http://mesos.apache.org/documentation/latest/upgrades/) for upgrading schedulers.
* Rate-limiting slave removal, to safeguard against unforeseen bugs which may lead to widespread slave removal.
* Support for module hooks in task launch sequence. Refer to the [modules documentation](http://mesos.apache.org/documentation/latest/modules/) for more information.
* Anonymous modules: a new kind of module that does not receive any callbacks but coexists with its parent process.
* New service discovery info in task info allows framework users to specify discoverability of tasks for external service discovery systems.
  Refer to the [framework development guide](http://mesos.apache.org/documentation/latest/app-framework-development-guide/) for more information.

Furthermore, many bugs have been fixed in this release. For full release notes with all features and bug fixes, please refer to the [CHANGELOG](https://github.com/apache/mesos/blob/master/CHANGELOG).

Rolling upgrades may be done seamlessly, however some things have changed in 0.22.0 with regard to external tooling and upgrade paths; please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to this release from a 0.21.1 cluster.

## Contributors

Thanks to the 35 contributors who made 0.22.0 possible:
Adam B, Alexander Rojas, Alexander Rukletsov, Ankur Chauhan, Benjamin Hindman, Benjamin Mahler, Bernd Mathiske, Chengwei Yang, Chi Zhang, Christos Kozyrakis, Cody Maloney, Connor Doyle, Dario Rexin, Dave Lester, David Robinson, Dominic Hamon, Evelina Dumitrescu, Gabriel Monroy, Ian Downes, Jake Farrell, Jiang Yan Xu, Jie Yu, Joerg Schad, Joris Van Remoortere, Kapil Arya, Kiyonari Harigae, Michael Park, Niklas Nielsen, Palak Choudhary, R.B. Boyer, Ryan Thomas, Samuel, Till Toenshoff, Timothy Chen and Vinod Kone.
