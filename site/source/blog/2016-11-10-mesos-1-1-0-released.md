---
layout: post
title: Apache Mesos 1.1.0 Released
permalink: /blog/mesos-1-1-0-released/
published: true
post_author:
  display_name: Till Toenshoff
  twitter: ttoenshoff
tags: Release
---

The latest Mesos release, 1.1.0, is now available for [download](http://mesos.apache.org/downloads).

This release includes the following features and improvements:

 * [MESOS-2449] - **Experimental** support for launching a group of tasks
   via a new `LAUNCH_GROUP` Offer operation. Mesos will guarantee that either
   all tasks or none of the tasks in the group are delivered to the executor.
   Executors receive the task group via a new `LAUNCH_GROUP` event.

 * [MESOS-2533] - **Experimental** support for HTTP and HTTPS health checks.
   Executors may now use the updated `HealthCheck` protobuf to implement
   HTTP(S) health checks. Both default executors (command and docker) leverage
   `curl` binary for sending HTTP(S) requests and connect to `127.0.0.1`,
   hence a task must listen on all interfaces. On Linux, for BRIDGE and USER
   modes, docker executor enters the task's network namespace.

 * [MESOS-3421] - **Experimental** Support sharing of resources across
   containers. Currently persistent volumes are the only resources allowed to
   be shared.

 * [MESOS-3567] - **Experimental** support for TCP health checks. Executors
   may now use the updated `HealthCheck` protobuf to implement TCP health
   checks. Both default executors (command and docker) connect to `127.0.0.1`,
   hence a task must listen on all interfaces. On Linux, for BRIDGE and USER
   modes, docker executor enters the task's network namespace.

 * [MESOS-4324] - Allow tasks to access persistent volumes in either a
   read-only or read-write manner. Using a volume in read-only mode can
   simplify sharing that volume between multiple tasks on the same agent.

 * [MESOS-5275] - **Experimental** support for linux capabilities. Frameworks
   or operators now have fine-grained control over the capabilities that a
   container may have. This allows a container to run as root, but not have all
   the privileges associated with the root user (e.g., CAP_SYS_ADMIN).

 * [MESOS-5344] - **Experimental** support for partition-aware Mesos
   frameworks. In previous Mesos releases, when an agent is partitioned from
   the master and then reregisters with the cluster, all tasks running on the
   agent are terminated and the agent is shutdown. In Mesos 1.1, partitioned
   agents will no longer be shutdown when they reregister with the master. By
   default, tasks running on such agents will still be killed (for backward
   compatibility); however, frameworks can opt-in to the new PARTITION_AWARE
   capability. If they do this, their tasks will not be killed when a partition
   is healed. This allows frameworks to define their own policies for how to
   handle partitioned tasks. Enabling the PARTITION_AWARE capability also
   introduces a new set of task states: TASK_UNREACHABLE, TASK_DROPPED,
   TASK_GONE, TASK_GONE_BY_OPERATOR, and TASK_UNKNOWN. These new states are
   intended to eventually replace the TASK_LOST state.

 * [MESOS-5788] - **Experimental** support for Java scheduler adapter. This
   adapter allows framework developers to toggle between the old/new API
   (driver/scheduler library) implementations, thereby allowing them to easily
   transition their frameworks to the new v1 Scheduler API.

 * [MESOS-6014] - **Experimental** A new port-mapper CNI plugin, the
   `mesos-cni-port-mapper` has been introduced. For Mesos containers, with the
   CNI port-mapper plugin, users can now expose container ports through host
   ports using DNAT. This is especially useful when Mesos containers are
   attached to isolated CNI networks such as private bridge networks, and the
   services running in the container needs to be exposed outside these
   isolated networks.

 * [MESOS-6077] - **Experimental** A new default executor is introduced which
   frameworks can use to launch task groups as nested containers. All the
   nested containers share resources likes cpu, memory, network and volumes.

#### Deprecations

 * The following metrics are deprecated and will be removed in Mesos 1.4:
     master/slave_shutdowns_scheduled,
     master/slave_shutdowns_canceled,
     slave_shutdowns_completed.
   As of Mesos 1.1.0, these metrics will always be zero. The following new
   metrics have been introduced as replacements:
     master/slave_unreachable_scheduled,
     master/slave_unreachable_canceled,
     master/slave_unreachable_completed.

 * [MESOS-5955] - Health check binary "mesos-health-check" is removed.

 * [MESOS-6371] - Remove the 'recover()' interface in 'ContainerLogger'.

#### Additional API Changes

 * [MESOS-6204] - A new agent flag called `--runtime_dir`. Unlike
   `--work_dir` which persists data across reboots, `--runtime_dir` is designed
   to checkpoint state that should persist across agent restarts, but not
   across reboots. By default this flag is set to `/var/run/mesos` when run as
   root and `os::temp/mesos/runtime/` when run as non-root.

 * [MESOS-6220] - HTTP handler failures should result in 500 rather than
   503 responses. This means that when using the master or agent endpoints,
   failures will now result in a `500 Internal Server Error` rather than a
   `503 Service Unavailable`.

 * [MESOS-6241] - New API calls (LAUNCH_NESTED_CONTAINER,
   KILL_NESTED_CONTAINER and WAIT_NESTED_CONTAINER) have been added to the
   v1 Agent API to manage nested containers within an executor container.

#### Unresolved Critical Issues

 * [MESOS-3794] - Master should not store arbitrarily sized data in ExecutorInfo.

 * [MESOS-4642] - Mesos Agent Json API can dump binary data from log files out as invalid JSON.

 * [MESOS-5396] - After failover, master does not remove agents with same UPID.

 * [MESOS-5856] - Logrotate ContainerLogger module does not rotate logs when run as root with `--switch_user`.

 * [MESOS-6142] - Frameworks may RESERVE for an arbitrary role.

 * [MESOS-6327] - Large docker images causes container launch failures: Too many levels of symbolic links.

 * [MESOS-6360] - The handling of whiteout files in provisioner is not correct.

 * [MESOS-6419] - The 'master/teardown' endpoint should support tearing down 'unregistered_frameworks'.

 * [MESOS-6432] - Roles with quota assigned can "game" the system to receive excessive resources.

#### All Experimental Features

 * [MESOS-2449] - Support group of tasks (Pod) constructs and API in Mesos.

 * [MESOS-2533] - Support HTTP checks in Mesos.

 * [MESOS-3094] - Mesos on Windows.

 * [MESOS-3421] - Support sharing of resources across task instances.

 * [MESOS-3567] - Support TCP checks in Mesos.

 * [MESOS-4312] - Porting Mesos on Power (ppc64le).

 * [MESOS-4355] - Implement isolator for Docker volume.

 * [MESOS-4641] - Support Container Network Interface (CNI).

 * [MESOS-4791] - Operator API v1.

 * [MESOS-4828] - XFS disk quota isolator.

 * [MESOS-5275] - Add capabilities support for unified containerizer.

 * [MESOS-5344] - Partition-aware Mesos frameworks.

 * [MESOS-5788] - Added JAVA API adapter for seamless transition to new scheduler API.

 * [MESOS-6014] - Added port mapping CNI plugin.

 * [MESOS-6077] - Added a default (task group) executor.

Furthermore, several bugfixes and improvements made it into this release.
For full release notes with all features and bug fixes, please refer to the [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.1.0).

### Upgrades

Rolling upgrades from a Mesos 1.0.0 cluster to Mesos 1.1.0 are straightforward. There are just some minor, backwards compatible deprecations.
Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.1.0.

### Try it out

We encourage you to try out this release and let us know what you think.
If you run into any issues, please let us know on the [user mailing list and IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 73 contributors who made 1.1.0 possible:

Aaron Wood, Abhishek Dasgupta, Alberto, Aleksandar Prokopec, Alex Clemmer, Alexander Rojas, Alexander Rukletsov, Ammar Askar, Anand Mazumdar, Andy Pang, Anindya Sinha, Artem Harutyunyan, Attila Szarvas, Avinash sridharan, Bart Spaans, Benjamin Bannier, Benjamin Hindman, Benjamin Mahler, Charles Allen, ChrisPaprocki, Daniel Pravat, Dario Rexin, Dave Lester, Gaojin CAO, Gastón Kleiman, Gilbert Song, Giulio Eulisse, Graham Taylor, Greg Mann, Guangya Liu, Haris Choudhary, Ian Babrou, Ilya Pronin, Jacob Janco, Jan Schlicht, Jay Guo, Jiang Yan Xu, Jie Yu, Joerg Schad, Joris Van Remoortere, Joseph Wu, Ken Sipe, Kevin Klues, Klaus Ma, Kunal Thakar, Lawrence Wu, Lijun Tang, Lukas Loesche, Mao Geng, Megha Sharma, Michael Park, Miguel Bernadin, Neil Conway, Pierre Cheynier, Qian Zhang, Roger Ignazio, Santhosh Kumar Shanmugham, Srinivas Brahmaroutu, Till Toenshoff, Tomasz Janiszewski, Vinod Kone, Will Rouesnel, Yong Tang, Yongqiao Wang, Yubo Li, Zhitao Li, anthony caiafa, gnolan, haosdent huang, janisz, uzyexe, zhou xing
