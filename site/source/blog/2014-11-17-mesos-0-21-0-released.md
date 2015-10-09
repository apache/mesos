---
layout: post
title: Mesos 0.21.0 Released
permalink: /blog/mesos-0-21-0-released/
published: true
post_author:
  display_name: Ian Downes
  twitter: ndwns
tags: Release, Containerizer, Modules, Reconciliation
---

The latest Mesos release, 0.21.0, is now available for [download](http://mesos.apache.org/downloads/). This version includes the following features and improvements:

* Task reconciliation for frameworks ([MESOS-1407](https://issues.apache.org/jira/browse/MESOS-1407))
* Support for Mesos modules ([MESOS-1931](https://issues.apache.org/jira/browse/MESOS-1931))
* Task status now includes source and reason ([MESOS-343](https://issues.apache.org/jira/browse/MESOS-343), [MESOS-1143](https://issues.apache.org/jira/browse/MESOS-1143))
* A shared filesystem isolator ([MESOS-1586](https://issues.apache.org/jira/browse/MESOS-1586))
* A pid namespace isolator ([MESOS-1765](https://issues.apache.org/jira/browse/MESOS-1765))

Full release notes are available in the release [CHANGELOG](https://github.com/apache/mesos/blob/master/CHANGELOG).

### State Reconciliation
Frameworks are now able to reconcile task state through the mesos API, to ensure that they remain eventually consistent in the face of failures. Read more about reconciliation [here](http://mesos.apache.org/documentation/latest/reconciliation/).

### Mesos Modules
Experimental support for Mesos modules was introduced in Mesos 0.21.0;
please note the [disclaimer](http://mesos.apache.org/documentation/latest/modules).

Mesos modules provide a way to easily extend inner workings of Mesos by creating and using shared libraries that are loaded on demand.  Modules can be used to customize Mesos without having to recompiling/relinking for each specific use case. Modules can isolate external dependencies into separate libraries, thus resulting into a smaller Mesos core. Modules also make it easy to experiment with new features. For example, imagine loadable allocators that contain a VM (Lua, Python, …) which makes it possible to try out new allocator algorithms written in scripting languages without forcing those dependencies into the project. Finally, modules provide an easy way for third parties to easily extend Mesos without having to know all the internal details.

For more details, please read the [modules documentation](http://mesos.apache.org/documentation/latest/modules)

### Task Status Includes Source and Reason
The `TaskStatus` associated with a status update has relied on a single enum and free-form string to communicate the status. There are benefits to adding more data to the status update, and in Mesos 0.21.0, we have introduced the notion of a `Source` and `Reason`.

The source of a status update is, broadly speaking, the actor that changed the status. This can currently be Master, Slave, or Executor.  Similarly, we have added an enumeration of reasons for the status update. The list can be found in `include/mesos/mesos.proto` and is fairly extensive.

Our hope is that frameworks can use the source and reason to better communicate status updates to end users, and make better scheduling decisions.

We have also introduced the notion of a `TASK_ERROR` state, distinct from `TASK_LOST`. The semantic difference is that tasks that are updated as lost can be rescheduled and should succeed, while tasks with status error will continue to fail if they are rescheduled. In Mesos 0.21.0 we do not send `TASK_ERROR` but it has been defined so frameworks can prepare to receive it. We will start sending it in Mesos 0.22.0.

### Shared Filesystem Isolator
Mesos 0.21.0 introduces an optional shared filesystem isolator for the Mesos Containerizer. The isolator does not provide full filesystem encapsulation like Docker. It is intended for deployments where containers share the host's root filesystem but certain parts of the filesystem should be made private to each container.

One example use-case is a private `/tmp` directory for each container.  Processes running in the container will see a normal `/tmp` directory but it actually refers to a subdirectory of the executor work directory, e.g., the relative path `./tmp`. Processes will not be able to see files in the host's `/tmp` directory or the `/tmp` directory of any other container.

This isolator is available only on Linux.

### Pid Namespace Isolator
Mesos 0.21.0 also introduces an optional pid namespace isolator for the Mesos Containerizer. The isolator runs each container in its own pid namespace such that processes inside a container will not have visibility to processes running on the host or in other containers.

The Mesos Containerizer Launcher has been updated to use the pid namespace to terminate all processes when destroying a container. This avoids known kernel bugs and race conditions when using the freezer cgroup. If the container is not running inside a pid namespace (started with an earlier slave version), the launcher will fall back to the prior behavior and use the freezer cgroup.

This isolator is only available on Linux.

### Getting Involved

We encourage you to try out this release and let us know what you think. If you run into any issues, please let us know on the [user mailing list and IRC](https://mesos.apache.org/community).

### Thanks

Thanks to the 27 [contributors](https://github.com/apache/mesos/graphs/contributors) who made 0.21.0 possible:

Adam Bordelon, Alexander Rukletsov, Benjamin Hindman, Benjamin Mahler, Bernd Mathiske, Chengwei Yang, Chi Zhang, Cody Maloney, Dave Lester, Dominic Hamon, Evelina Dumitrescu, Ian Downes, Jie Yu, Joe Gordon, Joris Van Remoortere, Dharmesh Kakadia, Kapil Arya, Michael Park, Niklas Q. Nielsen, R.B. Boyer, Thomas Rampelberg, Till Toenshoff, Timothy Chen, Timothy St. Clair, Tobi Knaup, Vinod Kone.
