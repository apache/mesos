## Networking support in Mesos

### Table of contents

- [Introduction](#introduction)
- [Attaching containers to IP networks](#attaching-containers)
  - [Mesos containerizer](#attaching-containers-mesos)
  - [Docker containerizer](#attaching-containers-docker)
  - [Limitations of Docker containerizer](#limitations-docker)
- [Retrieving network information for a container](#retrieve-network-info)


### <a name="introduction"></a>Introduction

Mesos supports two container runtime engines, the `MesosContainerizer`
and the `DockerContainerizer`. Both the container run time engines
provide IP-per-container support allowing containers to be attached to
different types of IP networks.  However, the two container run time
engines differ in the way IP-per-container support is implemented. The
`MesosContainerizer` uses the `network/cni` isolator to implement the
[Container Network Interface (CNI)](https://github.com/containernetworking/cni/blob/master/SPEC.md)
to provide networking support for Mesos containers, while the
`DockerContainerizer` relies on the Docker daemon to provide
networking support using Docker's [Container Network
Model](https://github.com/docker/libnetwork).

Note that while IP-per-container is one way to achieve network
isolation between containers, there are other alternatives to
implement network isolation within `MesosContainerizer`, e.g.,  using
the [port-mapping network isolator](isolators/network-port-mapping.md).

While the two container run-time engines use different mechanisms to
provide networking support for containers, the interface to specify
the network that a container needs to join, and the interface to
retrieve networking information for a container remain the same.

The `NetworkInfo` protobuf, described below, is the interface provided
by Mesos to specify network related information for a container and to
learn network information associated with a container.

```{.proto}
message NetworkInfo {
  enum Protocol {
    IPv4 = 1;
    IPv6 = 2;
  }

  message IPAddress {
    optional Protocol protocol = 1;
    optional string ip_address = 2;
  }

  repeated IPAddress ip_addresses = 5;
  optional string name = 6;
  repeated string groups = 3;
  optional Labels labels = 4;
};
```

This document describes the usage of the `NetworkInfo` protobuf, by
frameworks, to attach containers to IP networks. It also describes the
interfaces provided to retrieve IP address and other network related
information for a container, once the container has been attached to
an IP network.


### <a name="attaching-containers"></a>Attaching containers to IP networks

#### <a name="attaching-containers-mesos"></a>Mesos containerizer

`MesosContainerizer` has the [`network/cni`](cni.md) isolator enabled
by default, which implements CNI (Container Network Interface). The
`network/cni` isolator identifies CNI networks by using canonical
names. When frameworks want to associate containers to a specific CNI
network they specify a network name in the `name` field of the `
NetworkInfo` protobuf. Details about the configuration and interaction
of Mesos containers with CNI networks can be found in the
documentation describing ["CNI support for Mesos containers"](cni.md).


#### <a name="attaching-containers-docker"></a>Docker containerizer

Starting docker 1.9, there are four networking modes available in
Docker: NONE, HOST, BRIDGE and USER. ["Docker container
networks"](https://docs.docker.com/engine/userguide/networking/dockernetworks/)
provides more details about the various networking modes available in
docker. Mesos supports all the four networking modes provided by
Docker. To connect a docker container using a specific mode the
framework needs to specify the network mode in the `DockerInfo`
protobuf.

```{.proto}
message DockerInfo {
  // The docker image that is going to be passed to the registry.
  required string image = 1;

  // Network options.
  enum Network {
    HOST = 1;
    BRIDGE = 2;
    NONE = 3;
    USER = 4;
  }

  optional Network network = 2 [default = HOST];
 };

```

For `NONE`, `HOST`, and `BRIDGE` network mode the framework only needs
to specify the network mode in the `DockerInfo` protobuf. To use other
networks, such as `MACVLAN` on Linux, `TRANSPARENT` and `L2BRIDGE` on
Windows, or any other user-defined network, the network needs to be
created beforehand and the `USER` network mode needs to be chosen. For
the `USER` mode, since a user-defined docker network is identified by a
canonical network name (similar to CNI networks) apart from setting the
network mode in `DockerInfo` the framework also needs to specify the
`name` field in the `NetworkInfo` protobuf corresponding to the name of
the user-defined docker network.

Note that on Windows, the `HOST` network mode is not supported. Although the
`BRIDGE` network mode does not exist on Windows, it has an equivalent mode
called `NAT`, so on Windows agents, the `BRIDGE` mode will be interpretted as
`NAT`. If the network mode is not specified, then the default mode will be
chosen, which is `HOST` on Linux and `NAT` on Windows.

#### <a name="limitations-docker"></a>Limitations of Docker containerizer

One limitation that the `DockerContainerizer` imposes on the
containers using the USER network mode is that these containers cannot
be attached to multiple docker networks. The reason this limitation
exists is that to connect a container to multiple Docker networks,
Docker requires the container to be created first and then attached to
the different networks. This model of orchestration does not fit the
current implementation of the `DockerContainerizer` and hence the
restriction of limiting docker container to a single network.


### <a name="retrieve-network-info"></a>Retrieving network information for a container

Whenever a task runs on a Mesos agent, the executor associated with
the task returns a `TaskStatus` protobuf associated with the task.
Containerizers (Mesos or Docker) responsible for the container will
populate the `ContainerStatus` protobuf associated with the
`TaskStatus`. The `ContainerStatus` will contain multiple
`NetworkInfo` protobuf instances, one each for the interfaces
associated with the container. Any IP address associated with the
container will be reflected in the `NetworkInfo` protobuf instances.

The `TaskStatus` associated with each task can be accessed at the
Agent's `state` endpoint on which the task is running or it can be
accessed in the Master's `state` endpoint.
