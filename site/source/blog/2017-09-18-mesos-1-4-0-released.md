---
layout: post
title: Apache Mesos 1.4.0 Released
permalink: /blog/mesos-1-4-0-released/
published: true
post_author:
  display_name: Kapil Arya
  twitter: karya0
tags: Release
---

The latest Mesos release, 1.4.0, is now available for [download](/downloads). This release includes the following features and improvements:

  * [MESOS-5116](https://issues.apache.org/jira/browse/MESOS-5116) -
  The `disk/xfs` isolator now supports the `--enforce_container_disk_quota` flag
  to efficiently measure disk usage without enforcing usage constraints. The
  previous implementation always enforced the quota even if the operator might
  not have wanted it.

  * [MESOS-6223](https://issues.apache.org/jira/browse/MESOS-6223) -
  With partition awareness, the agents are now allowed to reregister after they
  have been marked Unreachable. Thus, they can recover their agent ID after a
  host reboot. See docs/upgrades.md for details.

  * [MESOS-6375](https://issues.apache.org/jira/browse/MESOS-6375) -
  *Experimental* Support for hierarchical resource
  allocation roles. Hierarchical roles allows delegation of resource
  allocation policies (i.e. fair sharing and quota) further down the
  hierarchy. For example, the "engineering" organization gets a 75%
  share of the resources, but it's up to the operators within the
  "engineering" organization to figure out how to fairly share between
  the "engineering/backend" team and the "engineering/frontend" team.
  The same delegation applies for quota. NOTE: There are known issues
  related to hierarchical roles (e.g. hierarchical quota allocation
  is not implemented and quota will be over-allocated if used with
  hierarchical roles, see: MESOS-7402) and thus it is not recommended
  for production usage at this time.

  * [MESOS-7418](https://issues.apache.org/jira/browse/MESOS-7418),
  [MESOS-7088](https://issues.apache.org/jira/browse/MESOS-7088) -
  File-based secrets are now supported for Mesos and Universal containerizer.
  A new SecretResolver module kind is introduced to fetch and resolve any
  secrets before they are made available to the tasks. Image-pull secrets are
  also supported for Docker registry credentials.

  * [MESOS-7477](https://issues.apache.org/jira/browse/MESOS-7477) -
  Linux ambient capabilites are now supported, so frameworks can run tasks that
  use ambient capabilites to grant limited additional privileged to tasks
  without the requirement for matching file-based capabilities.

  * [MESOS-7476](https://issues.apache.org/jira/browse/MESOS-7476),
  [MESOS-7671](https://issues.apache.org/jira/browse/MESOS-7671) -
  Support for frameworks and operators specifying Linux bounding capabilities in
  order to limit the maximum privileges that a task may acquire.

More than 200 other bug fixes and improvements made it into this release. For full release notes with all features and bug fixes, please refer to the [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.4.0).

### Upgrades

Rolling upgrades from a Mesos 1.3.0 cluster to Mesos 1.4.0 are straightforward. Please refer to the [upgrade guide](/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.4.0.

### Try it out

We encourage you to try out this release and let us know what you think.
If you run into any issues, please let us know on the [user mailing list, IRC, or Slack](/community).

### Thanks!

Thanks to the 53 contributors who made Mesos 1.4.0 possible:

Aaron Wood, Adam B, Alastair Montgomery, Alexander Rojas, Alexander Rukletsov, Anand Mazumdar, Andrei Budnik, Andrew Schwartzmeyer, Anindya Sinha, Armand Grillet, Avinash sridharan, Benjamin Bannier, Benjamin Hindman, Benjamin Mahler, Benno Evers, Chun-Hung Hsiao, Conrad Kleinespel, Dmitry Zhuk, Eric Chung, Gastón Kleiman, Gilbert Song, Greg Mann, haosdent huang, Haosdent Huang, hardy.jung, huangwenpeng, Jacob Janco, James Peach, Jan Schlicht, Jay Guo, Jeff Coffler, Jiang Yan Xu, Jie Yu, John Kordich, Joseph Wu, Kapil Arya, Kevin Klues, Li Li, Mao Geng, Megha Sharma, Michael Park, Neil Conway, Olivier Sallou, Qian Zhang, Quinn Leng, Shingo Kitayama, Silas Snider, Till Toenshoff, Tim Hansen, TobiLG, Vinod Kone, Zhitao Li, Zhongbo Tian
