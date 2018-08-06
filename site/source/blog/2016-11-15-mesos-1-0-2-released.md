---
layout: post
title: Apache Mesos 1.0.2 Released
permalink: /blog/mesos-1-0-2-released/
published: true
post_author:
  display_name: Vinod Kone
  twitter: vinodkone
tags: Release
---

The latest Mesos release, 1.0.2, is now available for [download](http://mesos.apache.org/downloads). This release includes some important bug fixes and improvements on top of 1.0.0. It is highly recommended to use this version if you are considering using Mesos 1.0. More specifically, this release includes the following fixes and improvements:


** Bug

[MESOS-4638](https://issues.apache.org/jira/browse/MESOS-4638) - Versioning preprocessor macros.

[MESOS-4973](https://issues.apache.org/jira/browse/MESOS-4973) - Duplicates in 'unregistered_frameworks' in /state

[MESOS-4975](https://issues.apache.org/jira/browse/MESOS-4975) - mesos::internal::master::Slave::tasks can grow unboundedly.

[MESOS-5613](https://issues.apache.org/jira/browse/MESOS-5613) - mesos-local fails to start if MESOS_WORK_DIR isn't set.

[MESOS-6013](https://issues.apache.org/jira/browse/MESOS-6013) - Use readdir instead of readdir_r.

[MESOS-6026](https://issues.apache.org/jira/browse/MESOS-6026) - Tasks mistakenly marked as FAILED due to race b/w sendExecutorTerminatedStatusUpdate() and _statusUpdate().

[MESOS-6074](https://issues.apache.org/jira/browse/MESOS-6074) - Master check failure if the metrics endpoint is polled soon after it starts.

[MESOS-6100](https://issues.apache.org/jira/browse/MESOS-6100) - Make fails compiling 1.0.1.

[MESOS-6104](https://issues.apache.org/jira/browse/MESOS-6104) - Potential FD double close in libevent's implementation of `sendfile`.

[MESOS-6118](https://issues.apache.org/jira/browse/MESOS-6118) - Agent would crash with docker container tasks due to host mount table read.

[MESOS-6122](https://issues.apache.org/jira/browse/MESOS-6122) - Mesos slave throws systemd errors even when passed a flag to disable systemd.

[MESOS-6152](https://issues.apache.org/jira/browse/MESOS-6152) - Resource leak in libevent_ssl_socket.cpp.

[MESOS-6212](https://issues.apache.org/jira/browse/MESOS-6212) - Validate the name format of mesos managed docker containers.

[MESOS-6216](https://issues.apache.org/jira/browse/MESOS-6216) - LibeventSSLSocketImpl::create is not safe to call concurrently with os::getenv.

[MESOS-6233](https://issues.apache.org/jira/browse/MESOS-6233) - Master CHECK fails during recovery while relinking to other masters.

[MESOS-6234](https://issues.apache.org/jira/browse/MESOS-6234) - Potential socket leak during Zookeeper network changes.

[MESOS-6245](https://issues.apache.org/jira/browse/MESOS-6245) - Driver based schedulers performing explicit acknowledgements cannot acknowledge updates from HTTP based executors.

[MESOS-6246](https://issues.apache.org/jira/browse/MESOS-6246) - Libprocess links will not generate an ExitedEvent if the socket creation fails.

[MESOS-6269](https://issues.apache.org/jira/browse/MESOS-6269) - CNI isolator doesn't activate loopback interface.

[MESOS-6274](https://issues.apache.org/jira/browse/MESOS-6274) - Agent should not allow HTTP executors to re-subscribe before containerizer recovery is done.

[MESOS-6324](https://issues.apache.org/jira/browse/MESOS-6324) - CNI should not use `ifconfig` in executors `pre_exec_command`.

[MESOS-6391](https://issues.apache.org/jira/browse/MESOS-6391) - Command task's sandbox should not be owned by root if it uses container image.

[MESOS-6393](https://issues.apache.org/jira/browse/MESOS-6393) - Deprecated SSL_ environment variables are non functional already.

[MESOS-6420](https://issues.apache.org/jira/browse/MESOS-6420) - Mesos Agent leaking sockets when port mapping network isolator is ON.

[MESOS-6446](https://issues.apache.org/jira/browse/MESOS-6446) - WebUI redirect doesn't work with stats from /metric/snapshot.

[MESOS-6457](https://issues.apache.org/jira/browse/MESOS-6457) - Tasks shouldn't transition from TASK_KILLING to TASK_RUNNING.

[MESOS-6461](https://issues.apache.org/jira/browse/MESOS-6461) - Duplicate framework ids in /master/frameworks endpoint 'unregistered_frameworks'.

[MESOS-6502](https://issues.apache.org/jira/browse/MESOS-6502) - _version uses incorrect MESOS_{MAJOR,MINOR,PATCH}_VERSION in libmesos java binding.

[MESOS-6527](https://issues.apache.org/jira/browse/MESOS-6527) - Memory leak in the libprocess request decoder.


** Improvement

[MESOS-6075](https://issues.apache.org/jira/browse/MESOS-6075) - Avoid libprocess functions in `mesos-containerizer launch`.

[MESOS-6299](https://issues.apache.org/jira/browse/MESOS-6299) - Master doesn't remove task from pending when it is invalid.


Full release notes are available in the release [CHANGELOG](https://gitbox.apache.org/repos/asf?p=mesos.git;a=blob_plain;f=CHANGELOG;hb=1.0.2)

### Upgrades

Rolling upgrades from a Mesos 1.0.0 cluster to Mesos 1.0.2 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.0.2.

### Try it out

We encourage you to try out this release and let us know what you think. If you run into any issues, please let us know on the [user mailing ld Slack/IRC](https://mesos.apache.org/community).

### Thanks!

Thanks to the 20 contributors who made 1.0.2 possible

Alexander Rukletsov, Ammar Askar, Anand Mazumdar, Avinash sridharan, Benjamin Bannier, Benjamin Hindman, Benjamin Mahler, Gastón Kleiman, Jiang Yan Xu, Jie Yu, Joris Van Remoortere, Joseph Wu, Kevin Klues, Manuwela Kanade, Neil Conway, Santhosh Kumar Shanmugham, Till Toenshoff, Vinod Kone, Zhitao Li, haosdent huang.
