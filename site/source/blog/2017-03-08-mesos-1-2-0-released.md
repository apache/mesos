---
layout: post
title: Apache Mesos 1.2.0 Released
permalink: /blog/mesos-1.2-0-released/
published: true
post_author:
  display_name: Adam
tags: Release
---

The latest Mesos release, 1.2.0, is now available for [download](/downloads). This release includes the following features and improvements:

  * [MESOS-5931](https://issues.apache.org/jira/browse/MESOS-5931) -
     **Experimental** support for auto backend in Mesos Containerizer,
    prefering overlayfs then aufs. Please note that the bind backend needs to be
    specified explicitly through the agent flag `--image_provisioner_backend`
    since it requires the sandbox already existed.

  * [MESOS-6402](https://issues.apache.org/jira/browse/MESOS-6402) -
    **Experimental** support for rlimit in Mesos containerizer.
    The isolator adds support for setting POSIX resource limits (rlimits) for
    containers launched using the Mesos containerizer. POSIX rlimits can be used
    to control the resources a process can consume. See
    [docs](/documentation/latest/isolators/posix-rlimits/) for details.

  * [MESOS-6419](https://issues.apache.org/jira/browse/MESOS-6419) -
    **Experimental**: Teardown unregistered frameworks. The master
    now treats recovered frameworks very similarly to frameworks that are registered
    but currently disconnected. For example, recovered frameworks will be reported
    via the normal "frameworks" key when querying HTTP endpoints. This means there
    is no longer a concept of "orphan tasks": if the master knows about a task, the
    task will be running under a framework. Similarly, "teardown" operations on
    recovered frameworks will now work correctly.

  * [MESOS-6460](https://issues.apache.org/jira/browse/MESOS-6460) -
    **Experimental**: Container Attach and Exec. This feature adds new
    [Agent APIs](/documentation/latest/operator-http-api) for attaching a remote
    client to the stdin, stdout, and stderr of a running Mesos task, as well as
    an API for launching new processes inside the same container as a running
    Mesos task and attaching to its stdin, stdout, and stderr. At a high level,
    these APIs mimic functionality similar to docker attach and docker exec.
    The primary motivation for such functionality is to enable users to debug
    their running Mesos tasks.

  * [MESOS-6758](https://issues.apache.org/jira/browse/MESOS-6758) -
    **Experimental** support for 'Basic' auth docker private registry
    on Mesos Containerizer. Until now, the mesos containerizer always assumed
    Bearer auth, but we now also support basic auth for private registries. Please
    note that the AWS ECS uses Basic authorization but it does not work yet due to
    the redirect issue [MESOS-5172](https://issues.apache.org/jira/browse/MESOS-5172).

More than 200 other bug fixes and improvements made it into this release. For full release notes with all features and bug fixes, please refer to the [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.2.0).

### Upgrades

Rolling upgrades from a Mesos 1.1.0 cluster to Mesos 1.2.0 are straightforward. There are just some minor, backwards compatible deprecations.
Please refer to the [upgrade guide](/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.2.0.

### Try it out

We encourage you to try out this release and let us know what you think.
If you run into any issues, please let us know on the [user mailing list and IRC](/community).

### Thanks!

Thanks to the 56 contributors who made Mesos 1.2.0 possible:

Aaron Wood, Abhishek Dasgupta, Adam B, Alexander Rojas, Alexander Rukletsov, Alex Clemmer, Anand Mazumdar, Andrew Schwartzmeyer, Anindya Sinha, Armand Grillet, Avinash Sridharan, Benjamin Bannier, Benjamin Hindman, Benjamin Mahler, Bruce Merry, Chengwei Yang, Daniel Pravat, David Forsythe, Dmitry Zhuk, Gastón Kleiman, Gilbert Song, Greg Mann, Guangya Liu, Haosdent Huang, Ilya Pronin, Jacob Janco, James Peach, Jan Schlicht, Jay Guo, Jeff Malnick, Jiang Yan Xu, Jie Yu, Joerg Schad Johannes Unterstein, Joris Van Remoortere, Joseph Wu, Kevin Klues, Lior Zeno, Manuwela Kanade, Megha Sharma, Michael Park, Miguel Bernadin, Neil Conway, Nicholas Sun, Qian Zhang, Ronald Petty, Santhosh Kumar Shanmugham, Sivaram Kannan, Srinivas Brahmaroutu, Thomas Maurice, Till Toenshoff, Tomasz Janiszewski, Vijay Srinivasaraghavan, Vinod Kone, Yubo Li, and Zhitao Li.
