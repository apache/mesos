---
layout: post
title: Apache Mesos 1.0.0 Released
permalink: /blog/mesos-1-0-0-released/
published: true
post_author:
  display_name: Vinod Kone
  twitter: vinodkone
tags: Release
---

Mesos 1.0 release is finally here for [download](http://mesos.apache.org/downloads)!

As most of you know, Apache Mesos has been running in production at some of the largest Internet companies like [Twitter](https://twitter.com), [Netflix](https://netflix.com), [Uber](https://uber.com) and [NASA JPL](http://www.jpl.nasa.gov/) for many years now. It has been proven to scale to tens of thousands of nodes, running hundreds of thousands of containers serving live traffic without interruption for months at a time.

Given its track record at these high profile companies, Apache Mesos could have reached its 1.0 milestone a long time ago. But one thing we really wanted to improve upon before doing a 1.0 release was to develop new HTTP APIs to ease the lives of both framework developers and cluster operators alike. These new APIs make it really easy to write a new framework or develop a new tool with off-the-shelf HTTP libraries written in the language of your choice. After iterating on the new APIs for nearly a year, we are confident that they are now ready for widespread use. Combined with a versioning scheme for the new APIs and a well-formulated [release and support process](http://mesos.apache.org/documentation/latest/versioning/), we are happy to call our latest release of Apache Mesos 1.0.

In addition to the new APIs, the 1.0 release is jam-packed with features that makes running stateless and stateful workloads easy. Below, we will discuss some of the highlights in this release. For further details please refer to the [CHANGELOG](https://github.com/apache/mesos/blob/1.0.0/CHANGELOG).

### HTTP API

Before 1.0, there were essentially two types of APIs exposed by Mesos -- the driver-based Framework API used by schedulers and executors and the REST-based HTTP API used by operators and tools. While the driver-based API made writing frameworks easier by hiding the complexities of interfacing with Mesos itself (e.g., leader detection, reliable reconnection, message delivery, etc.) it had several drawbacks. For example, only a few languages had bindings for the driver (specifically, C++, Java and Python), limiting the languages that frameworks could be written in. Moreover, the driver required both the client (e.g., scheduler) and the server (e.g., master) to open connections to one another in order to communicate. This requirement for a bi-directional connection between clients and servers made it really hard for clients to run inside containers or behind firewalls.

The new [HTTP API](https://www.youtube.com/watch?v=mDn_iyFSv38&index=13&list=PLGeM09tlguZQVL7ZsfNMffX9h1rGNVqnC) aims to solve these problems by providing an RPC-based HTTP API for both frameworks (i.e. [schedulers](http://mesos.apache.org/documentation/latest/scheduler-http-api/) and [executors](http://mesos.apache.org/documentation/latest/executor-http-api/)) and [operators](http://mesos.apache.org/documentation/latest/operator-http-api/). Since the API client libraries can be written in any language that speaks HTTP, it immediately opens the door for frameworks to be written in new languages. More importantly, the HTTP API doesn’t require servers to open a connection back to the client to communicate with it. As such, robust strategies for handling network partitions can now be implemented easily within the Mesos codebase itself.

Apart from simplifying the API for framework developers and operators, one of the most exciting new features in the new HTTP API is **experimental** support for [event streams](http://mesos.apache.org/documentation/latest/operator-http-api/). Event streams are a frequently requested -- and long awaited feature -- that have finally made their way into the Mesos codebase. Instead of continuously polling the heavy weight /state endpoint, clients (e.g, service discovery systems) can now get events streamed to them directly by the master.

### Unified Containerizer

Before the unified containerizer support was available, Mesos depended on external daemons to run containers based on a specific image format; `docker` daemon to run docker images, `rkt` to run appc images and so on. This made the system architecture complex and added more failure points to the platform. With the [unified containerizer](https://www.youtube.com/watch?v=rHUngcGgzVM&index=14&list=PLGeM09tlguZQVL7ZsfNMffX9h1rGNVqnC), we can now use a single container runtime engine, the `MesosContainerizer` to run containers off the most popular image formats (Docker, Appc, and soon OCI) without dependency on any external daemons. This makes the platform extremely robust, simplifies the system architecture and gives a unified isolation model for containers, allowing developers to achieve higher feature velocity in improving Mesos.

### Networking

With the addition of support for the Container Networking Interface ([CNI](https://github.com/containernetworking/cni)) in 1.0, we have made significant strides towards a comprehensive networking story in Mesos. Now, each container gets its own network namespace, which provides network isolation and allows users to work with containers as if they were running on bare metal or VM. Without network isolation, users have to manage network resources such as TCP/UDP ports on the agent. Managing these resources complicates the design of applications significantly.

One of the highlights of the new CNI support is the ability to now provide a single IP per container in Mesos.  This is one of the most requested features that Mesos has ever had!! Since the CNI interface is widely supported by networking vendors, this gives Mesos tremendous flexibility in orchestrating containers on a wide variety of networking technologies (e.g, VxLAN, [DC/OS overlay](https://dcos.io/docs/1.8/administration/overlay-networks/), [Calico](https://www.projectcalico.org/), [Weave](https://github.com/weaveworks/weave-cni), [Flannel](https://github.com/containernetworking/cni/blob/master/Documentation/flannel.md)).

### Storage
Starting from Mesos 1.0, we added **experimnental support** for external storage to Mesos. Before this feature, while users could use persistent volumes for running stateful services, there were some limitations. First, the users were not able to easily use non-local storage volumes. Second, data migrations for local persistent volumes had to be manually handled by operators. The newly added `docker/volume` isolator addresses these limitations. Currently, the isolator interacts with the Docker volume plugins (e.g., [REX-Ray](https://github.com/emccode/rexray), [Flocker](https://github.com/ClusterHQ/flocker), [Convoy](https://github.com/rancher/convoy)) using a tool called `dvdcli`. By speaking the Docker volume plugin API, Mesos containers can connect with external volumes from numerous storage providers (e.g., Amazon EBS, Ceph, EMC ScaleIO).

### Security

Mesos 1.0 comes with significant improvements in authentication and authorization. While we have long supported framework authentication and SSL/TLS encryption for frameworks and REST endpoints, not all HTTP endpoints were authenticated or authorized. With 1.0, all the sensitive endpoints have support for authentication and authorization.

More importantly, we have added foundations for multi-tenancy by adding [fine-grained authorization](https://www.youtube.com/watch?v=-yWHuxXwuAA&index=20&list=PLGeM09tlguZQVL7ZsfNMffX9h1rGNVqnC) controls. For example, it is now possible to set up ACLs so that a user can only view information about her own tasks in the WebUI and/or HTTP endpoints. This is a must-have for enterprises that need to enforce strict policies around access to information. As part of this effort, the Authorizer interface has been completely overhauled to make it easy for module writers to plugin enterprise-specific security policies.

### GPU Support

That’s right, you can now run your [TensorFlow](https://www.tensorflow.org/) jobs [directly on Mesos](https://www.youtube.com/watch?v=giJ4GXFoeuA&list=PL903xBaKIoaWc0hnSVyAAGAJUPxq2MCd0). The custom GPU isolator handles all of the details of allocating GPUs to your application and making sure they are isolated from other jobs running on the system. All you have to worry about is making sure your newest deep learning algorithm is ready for deployment!

As a bonus, we mimic the functionality of [nvidia-docker](https://github.com/NVIDIA/nvidia-docker) so you can take your existing GPU docker containers and run them on Mesos without modification. This means you can test your applications locally on docker and be assured that they will run as expected once you deploy them on Mesos.

We are working closely with the [Spark](http://spark.apache.org/), [Marathon](https://mesosphere.github.io/marathon/), and [Aurora](http://aurora.apache.org/) teams to make scheduling your GPU workloads as easy as possible. We’ll keep you posted as more things develop!

### Windows Support

Mesos 1.0 comes with **experimental** support for running Mesos Agent on Windows. It is now possible to build Mesos Agent and all its dependencies on Windows using CMake and Microsoft Visual Studio compiler. We are actively working on a tighter integration of Mesos Agent with Windows Container API (scheduled for release later this year), as well as adding support for launching Docker containers. The recent [Mesoscon talk](https://www.youtube.com/watch?v=bbyK1EBxrek) gives an overview of the current state of this work as well as presents the roadmap for Mesos Agent on Windows.

### Community

We would like to take this opportunity to give a huge thank you to the incredible Mesos community, without which none of this would be possible. We are fortunate to have a dedicated and enthusiastic community that not only finds and reports bugs but contributes code. In fact, all the highlighted features described above were done in close collaboration with several independent members and organizations in the community (e.g, IBM, Microsoft, Nvidia). It is rewarding to see an ever growing list of organizations using Mesos in production at scale and [contributing back to the community](http://mesos.apache.org/blog/dev-community-status/).  Some of these companies have not only built frameworks but entire platforms (e.g., [Azure Container Service](https://azure.microsoft.com/en-us/documentation/videos/azurecon-2015-deep-dive-on-the-azure-container-service-with-mesos/), [DC/OS](https://dcos.io/), [Rancher](http://rancher.com/mesos/))  on top of Mesos, which is truly humbling.

If you are new to Mesos and interested in learning more, please [join us](http://mesos.apache.org/community/) on this incredible journey. We are nowhere close to done implementing all the things that we want to build in Mesos and we could always use more help.
