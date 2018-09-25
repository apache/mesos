---
layout: post
title: "Mesos 1.7: Improvements to Performance, Containerization and Running Multiple Frameworks"
published: true
post_author:
  display_name: Chun-Hung Hsiao & Gastón Kleiman
  gravatar: 9447267537939b034faa02cbe08fef20
  twitter: chhsia0
tags: Release
---

We are excited to announce that Apache Mesos 1.7.0 is now available for [download](/downloads). Please take a look at what's new in this release!

# New Features and Improvements

## Performance

In Mesos 1.7, we put a lot of emphasis on performance. Thanks to the community’s hard work, several performance issues that have been affecting users are significantly improved:

![Mesos 1.7 performance improvements](/assets/img/blog/mesos-1-7-0-performance.png)

* The master `/state` endpoint is now about twice as fast as before, and its processing is parallelized to provide higher throughput. As a result, Mesos UI and all tooling that consumes the `/state` endpoint are more responsive.
* [Mesos containerizer](http://mesos.apache.org/documentation/latest/mesos-containerizer/) has been a high-performance alternative to the Docker runtime, and now it’s even faster: launch and destroy throughput has increased by 2x in this release.
* The agent `/containers` endpoint no longer leads to high CPU consumption and thus is much more responsive. It now scales well to hundreds of containers running on the agent.
* We made several optimizations to the resource allocator so the allocation cycle is now 20% faster on a 1,000-node cluster, and more significant improvements in this area are in progress for the 1.8 release.

In addition, there are also a variety of other minor performance improvements across the system. A separate blog post that describes the 1.7.0 performance improvements in-depth will soon be published, so stay tuned!

## Containerization

As usual, the 1.7.0 release includes several bug fixes to improve stability and reliability for users, as well as several new features and improvements:

* New `cgroups/all` agent isolation option: This option enables the cgroups isolator to detect supported cgroup subsystems automatically, so users no longer need to manually choose which cgroup subsystems to enable.
* Better cgroupfs isolation: Containers launched by [Mesos Containerizer](http://mesos.apache.org/documentation/latest/mesos-containerizer/) can now only see their own cgroups within `/sys/fs/cgroup`, and thus no container is allowed to see the cgroup information of other containers.
* New `linux/devices` isolator: This isolator automatically populates containers with devices that have been whitelisted in the `--allowed_devices` agent flag.
* Fetching Docker image tarballs from HDFS: Now users can specify an `hdfs://` URI in the `--docker_registry` agent flag to use HDFS in lieu of a Docker registry.
* Better container network statistics: The built-in Container Network Interface (CNI) isolator now exposes more statistics to make users easier to monitor the network usage of each container.
* Better image pulling metrics: The new [containerizer metrics](http://mesos.apache.org/documentation/latest/monitoring/#containerizers) enable users to monitor the latency distribution for image pulling.

## Framework Metrics

To improve the user experience of operators when monitoring and diagnosing a cluster, we have added a variety of new metrics in Mesos 1.7 to provide greater visibility into the behavior of individual frameworks in the cluster.

These metrics are added on a per-framework basis, so that new metric keys appear as new frameworks register with the master. Example metrics include: the numbers of calls received, number of events sent, offer-related counters (how many were sent/accepted/declined), counts of the various offer operations performed, and current task state totals for the framework. Please see the [metrics documentation](http://mesos.apache.org/documentation/latest/monitoring/#frameworks) for more details.

## Multi-Framework Workloads

The following improvements, combined with the allocator and metrics improvements mentioned above, make the experience of running multiple frameworks significantly better in Mesos 1.7:

* We have improved the [framework development guide](http://mesos.apache.org/documentation/latest/app-framework-development-guide/) with guidelines on how to implement frameworks that are well-behaved in a multi-framework cluster.
* A new weighted random sorter was also added as an alternative to the existing DRF sorter, allowing users that don't need DRF behavior to opt-out in favor of allocating to roles using a weighted uniform distribution.

# Upgrade

In most cases, upgrades from Mesos 1.6.0 to Mesos 1.7.0 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.7.0.

# Community

Inspired by the work that went into this release? Want to get involved? Have feedback? We'd love to hear from you! Join a [working group](http://mesos.apache.org/community/#working-groups) or start a conversation in the [community](http://mesos.apache.org/community/)!

# Thank you!

Thanks to the 50 contributors who made Mesos 1.7.0 possible:

Akash Gupta, Alexander Rojas, Alexander Rukletsov, Andrei Budnik, Andrew Schwartzmeyer, Armand Grillet, Benjamin Bannier, Benjamin Mahler, Benno Evers, Chen Runcong, Chun-Hung Hsiao, Clement Michaud, Dario Rexin, Eric Chung, Gary Yao, Gastón Kleiman, Gilbert Song, Greg Mann, Harold Dost, Ilya Pronin, James Peach, Jan Schlicht, Jiang Yan Xu, Jie Yu, John Kordich, Joseph Wu, Judith Malnick, Kapil Arya, Kevin Klues, Kjetil Joergensen, Meng Zhu, Pavi Sandhu, Qian Zhang, Radhika Jandhyala, Sergey Urbanovich, Tarun Gupta Akirala, Thodoris Zois, Till Toenshoff, Vinod Kone, Xudong Ni, Zhitao Li, Bin Zheng, Cai Shuhua, Cui Dt, He Yi Hua, janisz, LongFei Niu, Wei Xiao, Xiang Chaosheng, Zois
