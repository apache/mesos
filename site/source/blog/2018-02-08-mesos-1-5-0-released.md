---
layout: post
title: "Mesos 1.5: Storage, Performance, Resource Management, and Containerization improvements"
published: true
post_author:
  display_name: Gilbert Song
  gravatar: 05d3596cf7ef7751c02545b1eaac64ac
  twitter: Gilbert_Songs
tags: CSI, Storage, Containerization, Windows, Quota, Performance
---

I'm pleased to announce that Apache Mesos 1.5.0 was released today!

# New Features and Improvements

Mesos 1.5 includes significant improvements to resource management, storage and containerization. Container Storage Interface (CSI) support is new and experimental in Mesos 1.5, and many of the 1.5 features were added to support CSI. The new ability to reconfigure agents has made the operator experience better. Performance gains have made master failover 80-85% faster, and the v1 API performance has improved significantly. Resource management is more flexible with the new release. Last but not least, image garbage collection and better Windows support make it easier to run Mesos on different types of infrastructure, and to run different types of workloads on Mesos.

Read about all these improvements below, and enjoy Mesos 1.5.

## Container Storage Interface Support

Mesos 1.5 adds experimental support for the [Container Storage Interface (CSI)](https://github.com/container-storage-interface/spec).CSI is a specification that defines a common set of APIs for all interactions between storage vendors and container orchestration platforms. It is the result of a close collaboration among representatives from the Mesos, [Kubernetes](https://kubernetes.io/), [Cloud Foundry](https://www.cloudfoundry.org/), and [Docker](https://www.docker.com/) communities. The primary goal of CSI is to allow storage vendors to write one plugin that works with all container orchestration platforms.

Supporting CSI will help Mesos consistently fit into the larger storage ecosystem, resulting in better storage support in Mesos. Users will be able to use any storage system with Mesos using a consistent API. The out-of-tree plugin model of CSI decouples the release cycle of Mesos from that of the storage systems, making the integration itself more sustainable and maintainable.

![CSI Support in Mesos](/assets/img/documentation/csi-architecture.png)

The above figure shows the high-level architecture of how CSI is supported in Mesos. For more details, refer to this [documentation](http://mesos.apache.org/documentation/latest/csi/).

## Improved Operator Experiences

### Agent Reconfiguration Policy

Prior to Mesos 1.5, it was not possible to make any changes to the configuration of an agent without killing all tasks running on that agent and restarting with a new agent ID. Any change, for example, an additional attribute or a newly connected hard-drive, would be detected and abort the agent startup with a nasty-looking error:

```
EXIT with status 1: Failed to perform recovery: Incompatible agent info detected.
------------------------------------------------------------
[...]
------------------------------------------------------------
To remedy this do as follows:
Step 1: rm -f /path/to/work_dir/meta/slaves/latest
    This ensures agent doesn't recover old live executors.
Step 2: Restart the agent.
```

Starting with 1.5, operators can use the new agent command-line flag `--reconfiguration_policy` to configure the types of operations that should be allowed on agents, and which should lead to errors. When setting the value of this flag to `additive`, the agent will tolerate any changes that increase the amount of resources on the agent, as well as changes that add additional attributes or fault domains.

In the future, we hope to provide the additional setting `--reconfiguration_policy=any` to allow any changes to the agent configuration.

## Performance improvements

This release includes improvements to master failover and v1 operator state query API performance. Master failover time-to-completion achieved a 450-600% improvement in throughput, which reduces the time-to-completion by 80-85%.

![1.3 - 1.5 Master Failover with Task History Graph](/assets/img/documentation/1.3-1.5_master_failover_with_history.png)


See [our blog post](http://mesos.apache.org/blog/performance-working-group-progress-report/) for more details.

In addition, the performance of v1 operator state query API has also been greatly improved thanks to unnecessary copy eliminations. Specifically, v1 protobuf GetState call is 36% faster compared to v0. And, v1 JSON GetState performance has improved by more than four times.

![1.3 - 1.5 v1 JSON State Query Latency](/assets/img/documentation/1.3-1.5_v1_json_state_query_latency.png)

Detailed benchmark results can be found in [this spreadsheet](https://docs.google.com/spreadsheets/d/1mN4OLEi7UdbLxv-Q3k3iMlmO6Gb2wx9X-gHRnDENSK0/edit#gid=1562845084).

## Resource Management

### Quota guarantee improvements

Several quota related improvements were made in this release. Now Mesos does a better job ensuring that a role receives its quota and that a role does not exceed its quota, e.g.:

* Previously a role could “game” the quota system by amassing reservations that it leaves unused. This is now prevented by [accounting for reservations](https://issues.apache.org/jira/browse/MESOS-4527) when allocating resources.
* Resources are now [allocated in a fine-grained manner](https://issues.apache.org/jira/browse/MESOS-7099) to prevent roles from exceeding their quota.
* There was a [bug](https://issues.apache.org/jira/browse/MESOS-8293) where roles without quota may not receive their reservations. This has been fixed.
* When a role had more reservations than its quota, there was a [bug](https://issues.apache.org/jira/browse/MESOS-8339) where an insufficient amount of quota headroom was held. This has been fixed.
* When allocating resources to a role with quota, we previously also included resources that the role did not have the quota for on the agent. This made it possible to violate the quota guarantees of a different role. This has been fixed by [taking into account the headroom](https://issues.apache.org/jira/browse/MESOS-8352) that is needed when allocating the resources.

### Resource Provider abstraction

To decouple resource and agent life cycles we have introduced an abstraction for [resource providers](https://issues.apache.org/jira/browse/MESOS-7235). This lays the groundwork to support dynamic changes to agent resources, integration of external resource providers, or management of cluster-wide resources. As part of the decoupling of resource responsibilities, we have introduced a new protocol to distribute operations on resources. This allows them to explicitly communicate success or failure, and enables reconciliation between cluster components. We have used the new resource provider abstraction to integrate agent-local storage resources interfaced with CSI.

## Containerization and Multi-Platform Support

### Windows Support Improvements

With the release of Mesos 1.5, Windows support has been vastly improved. Now when running tasks with the Mesos containerizer, resource limits can be enforced at the operating system level via job objects ([MESOS-6690](https://issues-test.apache.org/jira/browse/MESOS-6690)). When the [windows/cpu](https://mesos.apache.org/documentation/latest/isolators/windows/#cpu-limits) and [windows/mem](https://mesos.apache.org/documentation/latest/isolators/windows/#memory-limits) isolators are used, a hard cap on the CPU cycles and virtual memory can be enforced. Additionally, the CPU and memory statistics are now correctly supported. The Mesos fetcher has been [ported to Windows](https://issues-test.apache.org/jira/browse/MESOS-8180), with support for automatically downloading files over TLS, and extracting ZIP archives. When configured for it, libprocess can now be built with [OpenSSL support on Windows](https://issues-test.apache.org/jira/browse/MESOS-7992), allowing TLS connections to be enforced within the cluster (note that this relies on building or installing OpenSSL externally, as it is not shipped with Windows). The first authentication method, CRAM-MD5, has been enabled on Windows, so that agents can authenticate with masters. Both HTTP and TCP [health checks](https://issues-test.apache.org/jira/browse/MESOS-6709) are now available for tasks on Windows agents (for shell tasks at least; health checks for Docker containers are coming soon). The Mesos agent no longer has to be run as an administrator, as it is now possible to [create symlinks as a normal user](https://issues-test.apache.org/jira/browse/MESOS-7370), and this feature was integrated into the agent. Finally, while this was available in the previous release, [native long path support for Windows](https://issues-test.apache.org/jira/browse/MESOS-7371) has been implemented, so that running the agent on Windows is now straightforward (with no more registry configurations).

### Container Image GC

Mesos 1.5 has support for Container Image Garbage Collection. There are two ways to garbage collect unused image layers: automatically or manually. This feature helps users avoid unbounded disk space usage in the docker image store. Please see [Container Image GC Document](http://mesos.apache.org/documentation/latest/container-image/#garbage-collect-unused-container-images) for more details.

### Standalone Container

This release includes a set of new operator APIs for launching and managing a new primitive called a "Standalone Container". A Standalone Container is similar to a container launched by a framework on the Mesos Agent, except that Standalone Containers are launched directly on the Mesos Agent by the operator. As a result, these containers do not use a Mesos Executor and have some other limitations (such as not being able to use reserved resources). At the same time, Standalone Containers are isolated like any other container and can use the same set of containerization features, like container images.

This feature is being released to support the [CSI](http://mesos.apache.org/documentation/latest/csi/) workflow but the APIs are not restricted to this use case. This feature may potentially be used to implement [daemon processes for agent](https://issues.apache.org/jira/browse/MESOS-6595) nodes in future. See [MESOS-7302](https://issues.apache.org/jira/browse/MESOS-7302) for more details.

## Mesos Internals

### Support gRPC Client

This release includes a gRPC client wrapper. [gRPC](https://grpc.io/) is an RPC mechanism that is becoming increasingly popular in the cloud-native space as an alternative to REST API. As a first step to integrating gRPC into Mesos, we have added this client wrapper in libprocess to support CSI. This feature is disabled by default, and can be enabled with the `--enable-grpc` (autotools) build flag on clusters that have been built with the [OpenSSL](https://www.openssl.org/) library installed. The wrapper class maintains gRPC runtime data structures internally, and provides a simple interface to make asynchronous gRPC calls that are compatible to libprocess’ actor-based model. We have used this new feature to interface with CSI plugins. In addition, people can also use this new feature to create Mesos modules to talk to their own gRPC-based services.

### Replicated log improvements

Mesos replicated log is used by frameworks (e.g. Apache Aurora) to persist their state across multiple replicas so it can survive failovers and restarts. Starting with 1.5 it is possible to perform eventually consistent reads from a non-leading VOTING replicated log replica. This makes possible to do additional work on non-leading framework replicas, such as offloading some reading from a leader to standbys, or reducing failover time by keeping in-memory storage represented by the replicated log "hot".

# Upgrade

Rolling upgrades from a Mesos 1.4.0 cluster to Mesos 1.5.0 are straightforward. Please refer to the [upgrade guide](http://mesos.apache.org/documentation/latest/upgrades/) for detailed information on upgrading to Mesos 1.5.0.

# Community

Inspired by the work that went into this release? Want to get involved? Have feedback? We'd love to hear from you! Join a [working group](http://mesos.apache.org/community/#working-groups) or start a conversation in the [community](http://mesos.apache.org/community/)!

# Thank you!

Thanks to the 59 contributors who made Mesos 1.5.0 possible:

Aaron Wood, Adam B, Adam Dangoor, Akash Gupta, Alastair Montgomery, Alexander Rojas, Alexander Rukletsov, Anand Mazumdar, Andrei Budnik, Andrew Schwartzmeyer, Andrey Dudin, Andy Pang, Anindya Sinha, Armand Grillet, Avinash sridharan, Benjamin Bannier, Benjamin Hindman, Benjamin Mahler, Benno Evers, Bob Eckert, Chun-Hung Hsiao, Cynthia Thomas, Deepak Goel, Dmitry Zhuk, Gaston Kleiman, Gastón Kleiman, Gilbert Song, Greg Mann, Ilya Pronin, Jack Yang, James DeFelice, James Peach, Jan Schlicht, Jason Lai, Jeff Coffler, Jiang Yan Xu, Jie Yu, Joerg Schad, John Kordich, Joseph Wu, Julien Pepy, Kapil Arya, Kevin Klues, Megha Sharma, Meng Zhu, Michael Park, Nathan Jackson, Neil Conway, Packt, Pranay Kanwar, Qian Zhang, Quinn Leng, Richard Shaw, ThodorisZois, Till Toenshoff, Tomas Barton, Tomasz Janiszewski, Vinod Kone, Zhitao Li

Enjoy Apache Mesos 1.5!
