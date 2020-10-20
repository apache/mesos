---
layout: post
title: "Apache Mesos 1.10: Container Resource Bursting, Executor Domain Sockets, V1 Operator API Performance, and Reservation Update"
permalink: /blog/mesos-1-10-0-released/
published: true
post_author:
  display_name: Benjamin Mahler
  twitter: bmahler
tags: Release
---

We are excited to announce that Apache Mesos 1.10.0 has been released! Along with lots of bug fixes and improvements, the following are the larger items:

# New Features and Improvements

## Container Resource Bursting

Mesos now supports per-container CPU and memory usage beyond the allocation to the container, up to a configured limit. Previously, it was only possible to enable CPU bursting for all (or none) of the containers on the agent, without per-container control.

This feature is useful for batch style workloads that can benefit from getting access to any unused CPU or memory on the agent. It’s also helpful for increasing utilization of the machines in a cluster, by allowing unallocated and/or unused CPU to be utilized.

Frameworks specify CPU and memory limits for tasks (separately from resource requests) and also the level of isolation they desire when launching task groups - CPU and memory may be isolated at the executor container level, or the task container level.

See [here](http://mesos.apache.org/documentation/latest/running-workloads/) for more information.

## Executor Domain Sockets

When a task is launched in Mesos, the executor is expected to connect to the agent through a TCP socket in order to register itself. This makes it necessary to allow TCP connections between the environment of the executor and the host network of the mesos agent. In some contexts (e.g. containers using CNI networks), it is desirable to firewall any TCP connections to the agent host. To support this use-case, Mesos 1.10.0 introduces the ability for executors to communicate with agents on the same host using unix domain sockets.

See [here](http://mesos.apache.org/documentation/latest/executor-http-api/) for more information on how to use this feature.

## V1 Operator API Performance

V1 operator API performance has lagged behind the V0 operator API, as the V0 operator API underwent significant performance optimizations in recent releases. This has made it impossible to recommend the V1 operator API to users given its scalability issues.

Performance of read-only V1 operator API calls has been improved by introducing direct serialization into JSON/protobuf and extending the batching mechanism to parallel processing of these calls by the master (similarly to `/state` endpoint). The V1 operator API performance is now on par with the V0 HTTP endpoints.

## Reservation Update

Reservations in Mesos control which frameworks can consume resources, e.g., operators use reservations to partition clusters, or frameworks use DYNAMIC reservations to ensure that resources are not offered elsewhere should a task fail over. In addition, persistent volumes are bound to a reservation.

Existing reservations can now be updated via the RESERVE_RESOURCES master API call. This, for example, allows the operator to move persistent volumes between roles non-destructively.

An example is shown [here](http://mesos.apache.org/documentation/latest/operator-http-api/#reserve_resources).

# Upgrade

Upgrades from Mesos 1.9.0 to Mesos 1.10.0 should be straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.10.0.

# Community

Inspired by the work that went into this release? Want to get involved? Have feedback? We'd love to hear from you! Join a [working group](http://mesos.apache.org/community/#working-groups) or start a conversation in the [community](http://mesos.apache.org/community/)!

# Thank you!

Special thanks to Andrei Sekretenko as the release manager.

Thanks to the 24 contributors who made Mesos 1.10.0 possible:

Adam Cecile, Aleksandr Kuzmitsky, Andrei Budnik, Andrei Sekretenko, Benjamin Bannier, Benjamin Mahler, Benno Evers, Bo Anderson, Charles-Francois Natali, Chun-Hung Hsiao, Damien Gerard, Dominik Dary, Dong Zhu, Greg Mann, Grégoire Seux, James Peach, James Wright, Jonathan Robson, Joseph Wu, Maxime Brugidou, Meng Zhu, Qian Zhang, Vinod Kone
