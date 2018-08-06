---
layout: post
title: Apache Mesos 1.3.0 Released
permalink: /blog/mesos-1-3-0-released/
published: true
post_author:
  display_name: Neil Conway
  twitter: neil_conway
tags: Release
---

The latest Mesos release, 1.3.0, is now available for [download](/downloads). This release includes the following features and improvements:

  * [MESOS-1763](https://issues.apache.org/jira/browse/MESOS-1763) -
    Support for frameworks to receive resources for multiple roles. This allows
    "multi-user" frameworks to leverage the role-based resource allocation in
    mesos. Prior to this support, one had to run multiple instances of a
    single-user framework to achieve multi-user resource allocation, or
    implement multi-user resource allocation in the framework.

  * [MESOS-6365](https://issues.apache.org/jira/browse/MESOS-6365) -
    Authentication and authorization support for HTTP executors.  A new
    `--authenticate_http_executors` agent flag enables required authentication
    on the HTTP executor API. A new `--executor_secret_key` flag sets a key file
    to be used when generating and authenticating default tokens that are passed
    to HTTP executors. Note that enabling these flags after upgrade is
    disruptive to HTTP executors that were launched before the upgrade; see
    'docs/authentication.md' for more information on these flags and the
    recommended upgrade procedure. Implicit authorization rules have been added
    which allow an authenticated executor to make executor API calls as that
    executor and make operator API calls which affect that executor's
    container. See 'docs/authorization.md' for more information on these
    implicit authorization rules.

  * [MESOS-6627](https://issues.apache.org/jira/browse/MESOS-6627) - Support for
    frameworks to modify the role(s) they are subscribed to. This is essential
    to supporting "multi-user" frameworks (see
    [MESOS-1763](https://issues.apache.org/jira/browse/MESOS-1763)) in that
    roles are expected to come and go over time (e.g. new employees join, new
    teams are formed, employees leave, teams are disbanded, etc).

More than 140 other bug fixes and improvements made it into this release. For full release notes with all features and bug fixes, please refer to the [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.3.0).

### Upgrades

Rolling upgrades from a Mesos 1.2.0 cluster to Mesos 1.3.0 are straightforward. Please refer to the [upgrade guide](/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.3.0.

### Try it out

We encourage you to try out this release and let us know what you think.
If you run into any issues, please let us know on the [user mailing list, IRC, or Slack](/community).

### Thanks!

Thanks to the 55 contributors who made Mesos 1.3.0 possible:

Aaron Wood, Abhishek Dasgupta, Adam B, Alex Clemmer, Alexander Rojas, Alexander Rukletsov, Anand Mazumdar, Andrew Schwartzmeyer, Andy Pang, Anindya Sinha, Anthony Sottile, Armand Grillet, Avinash sridharan, Ayanampudi Varsha, Benjamin Bannier, Benjamin Mahler, Benno Evers, Budnik Andrei, Chris Cao, Chun-Hung Hsiao, Daniel Pravat, Deshi Xiao, Dmitry Zhuk, Eric Chung, Gastón Kleiman, Gilbert Song, Greg Mann, Guangya Liu, Haosdent Huang, Haris Choudhary, Ilya Pronin, James Peach, Jan Schlicht, Jay Guo, Jeff Coffler, Jiang Yan Xu, Jie Yu, John Kordich, Joseph Wu, Kamil Wargula, Kapil Arya, Kevin Klues, Klaus Ma, Michael Park, Neil Conway, Pierre Cheynier, Qian Zhang, Srinivas Brahmaroutu, Till Toenshoff, Tomasz Janiszewski, Vinod Kone, Yan Xu, Zhitao Li, Zhongbo Tian, and ondrej.smola.
