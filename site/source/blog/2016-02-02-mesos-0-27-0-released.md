---
layout: post
title: Apache Mesos 0.27.0 Released
permalink: /blog/mesos-0-27-0-released/
published: true
post_author:
  display_name: Kapil Arya
  twitter: karya0
tags: Release
---

The latest Mesos release, 0.27.0, is now available for [download](http://mesos.apache.org/downloads).
This release includes the following features and improvements:

* [[MESOS-1791]](https://issues.apache.org/jira/browse/MESOS-1791): Support for _resource quota_ that provides non-revocable resource guarantees without tying reservations to particular Mesos agents.  Please refer to [the quota documentation](http://mesos.apache.org/documentation/latest/quota) for more information.

* [[MESOS-191]](https://issues.apache.org/jira/browse/MESOS-191): _Multiple disk_ support to allow for disk IO intensive applications to achieve reliable, high performance.

* [[MESOS-4085]](https://issues.apache.org/jira/browse/MESOS-4085): Flexible roles with the introduction of _implicit roles_. It deprecates the whitelist functionality that was implemented by specifying `--roles` during master startup to provide a static list of roles.

* [[MESOS-2353]](https://issues.apache.org/jira/browse/MESOS-2353): Performance improvement of the state endpoint for large clusters.

Furthermore, 167+ bugfixes and improvements made it into this release.
For full release notes with all features and bug fixes, please refer to the [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=0.27.0).

### Upgrades

Rolling upgrades from a Mesos 0.26.0 cluster to Mesos 0.27.0 are straightforward. There are just some minor, backwards compatible deprecations.
Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 0.27.0.


### Try it out

We encourage you to try out this release and let us know what you think.
If you run into any issues, please let us know on the [user mailing list and IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 55 contributors who made 0.27.0 possible:

Adam B, Alexander Rojas, Alexander Rukletsov, Alex Clemmer, Anand Mazumdar, Andy Pang, Anindya Sinha, Artem Harutyunyan, Avinash sridharan, Bartek Plotka, Benjamin Bannier, Benjamin Hindman, Benjamin Mahler, Bernd Mathiske, Bhuvan Arumugam, BrickXu, Chi Zhang, Cong Wang, Dario Rexin, David Forsythe, Diana Arroyo, Diogo Gomes, Felix Abecassis, Gilbert Song, Greg Mann, Guangya Liu, haosdent huang, James Peach, Jan Schlicht, Jian Qiu, Jie Yu, Jihun Kang, Jiri Simsa, Jocelyn De La Rosa, Joerg Schad, Jojy Varghese, Joris Van Remoortere, Joseph Wu, Kapil Arya, Kevin Klues, Klaus Ma, Mandeep Chadha, Michael Park, Neil Conway, Olivier Sallou, Qian Zhang, Shuai Lin, Stephan Erb, Steve Hoffman, Till Toenshoff, Timothy Chen, Vaibhav Khanduja, Vinod Kone, Vivek Juneja, and Zhitao Li.
