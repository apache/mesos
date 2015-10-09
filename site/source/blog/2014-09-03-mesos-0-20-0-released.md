---
layout: post
title: Mesos 0.20.0 Released
permalink: /blog/mesos-0-20-0-released/
published: true
post_author:
  display_name: Jie Yu
  twitter: jie_yu
tags: Release, Docker, Containerizer, Network Monitoring, Authorization
---

The latest Mesos release, 0.20.0, is now available for [download](http://mesos.apache.org/downloads/). This version includes the following features and improvements:

* Native Docker support ([MESOS-1524](https://issues.apache.org/jira/browse/MESOS-1524))
* Container level network monitoring ([MESOS-1228](https://issues.apache.org/jira/browse/MESOS-1228))
* Framework authorization ([MESOS-1342](https://issues.apache.org/jira/browse/MESOS-1342))
* Framework rate limiting ([MESOS-1306](https://issues.apache.org/jira/browse/MESOS-1306))
* Building against installed third-party dependencies ([MESOS-1071](https://issues.apache.org/jira/browse/MESOS-1071))

Full release notes are available in the release [CHANGELOG](https://github.com/apache/mesos/blob/master/CHANGELOG).

### Native Docker Support

Mesos 0.20.0 introduces first-class [Docker](https://www.docker.com/) support. Framework writers can now launch Docker containers for tasks by specifying `ContainerInfo` in `TaskInfo` or `ExecutorInfo`. The initial version allows the framework writers to specify Docker images for containers, and supports Docker primitives like volumes, entrypoints, etc.

Native Docker support is enabled by a new containerizer called DockerContainerizer, which can launch a Docker container as either a Task or an Executor. In 0.20.0, Mesos supports running multiple containerizers simultaneously and allows containerizers to be chosen dynamically based on task/executor info ([MESOS-1527](https://issues.apache.org/jira/browse/MESOS-1527)). Therefore, users can enable native Docker support without changing the containerizer strategy for their existing tasks.

Details about native Docker support can be found in the [Docker containerizer documentation](http://mesos.apache.org/documentation/latest/docker-containerizer/).

### Container Level Network Monitoring

Prior to Mesos 0.20.0, it was not possible to get the network statistics for each container, which made triaging or debugging network problems difficult in cases where multiple active tasks/executors were running on slave machines. Mesos 0.20.0 solves this problem by adding support for [per-container network monitoring](http://mesos.apache.org/documentation/latest/network-monitoring/). If you use the Mesos containerizer with the port_mapping isolator, network statistics for each active container can be retrieved through the `/monitor/statistics.json` endpoint on the slave.

This network monitoring solution is completely transparent to the tasks running on the slave. Tasks will not notice any difference to running on a slave without network monitoring enabled and were sharing the network of the slave. There is no need for users to change their service discovery mechanism.

### Framework Authorization

Authorization support has been implemented via Access Control Lists (ACLs). Authorization allows:

1. Frameworks to (re-)register with authorized ‘roles’
2. Frameworks to launch tasks/executors as authorized ‘users’
3. Authorized ‘principals’ to shutdown framework(s) through “/shutdown” HTTP endpoint

For instance, operators can use authorization to only allow framework "foo" (and no other frameworks) to launch executors as "root". For more details please read the [authorization documentation](http://mesos.apache.org/documentation/latest/authorization/).

### Framework Rate Limiting

In a multi-framework environment, [framework rate limiting](http://mesos.apache.org/documentation/latest/framework-rate-limiting/) aims to protect the throughput of high-SLA (e.g., production, service) frameworks by having the master throttle messages from other (e.g., development, batch) frameworks. Using this feature, an operator can specify the maximum number of queries per second and outstanding messages for the low-SLA frameworks on the master.

### Building Against Installed Third-party Dependencies

Mesos 0.20.0 now supports building using system installed dependencies. Prior to this release, most of the third-party dependencies of Mesos were included in the project and statically linked into the resulting binaries and libraries. See [MESOS-1071](https://issues.apache.org/jira/browse/MESOS-1071) for more details about these changes.

### Future Work

Looking forward, here are a few upcoming features that are being worked on:

* Improvement of Docker support in Mesos ([MESOS-1524](https://issues.apache.org/jira/browse/MESOS-1524))
* On top of network monitoring, evaluating and providing support for per-container network isolation ([MESOS-1585](https://issues.apache.org/jira/browse/MESOS-1585))
* Continued work on state reconciliation for frameworks ([MESOS-1407](https://issues.apache.org/jira/browse/MESOS-1407))

### Getting Involved

We encourage you to try out this release and let us know what you think. If you run into any issues, please let us know on the [user mailing list and IRC](https://mesos.apache.org/community).

### Thanks

Thanks to the 29 [contributors](https://github.com/apache/mesos/graphs/contributors) who made 0.20.0 possible:

Adam B, Alexandra Sava, Anton Lindström, Benjamin Hindman, Benjamin Mahler, Bernardo Gomez Palacio, Bill Farner, Chengwei Yang, Chi Zhang, Connor Doyle, Craig Hansen-Sturm, Dave Lester, Dominic Hamon, Gaudio, Ian Downes, Isabel Jimenez, Jiang Yan Xu, Jie Yu, Ken Sipe, Niklas Nielsen, Thomas Rampelberg, Timothy Chen, Timothy St. Clair, Tobias Weingartner, Tom Arnfeld, Vinod Kone, Yifan Gu, Zuyu Zhang
