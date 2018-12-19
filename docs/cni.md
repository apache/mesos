## Container Network Interface (CNI) for Mesos Containers

This document describes the `network/cni` isolator, a network isolator
for the [MesosContainerizer](mesos-containerizer.md) that implements
the [Container Network Interface
(CNI)](https://github.com/containernetworking/cni) specification.  The
`network/cni` isolator allows containers launched using the
`MesosContainerizer` to be attached to several different types of IP
networks.  The network technologies on which containers can possibly
be launched range from traditional layer 3/layer 2 networks such as
VLAN, ipvlan, macvlan, to the new class of networks designed for
container orchestration such as
[Calico](https://www.projectcalico.org/),
[Weave](https://weave.works/) and
[Flannel](https://coreos.com/flannel/docs/latest/).  The
`MesosContainerizer` has the `network/cni` isolator enabled by
default.

### Table of Contents
- [Motivation](#motivation)
- [Usage](#usage)
  - [Configuring CNI networks](#configuring-cni-networks)
  - [Adding/Deleting/Modifying CNI networks](#adding-modifying-deleting)
  - [Attaching containers to CNI networks](#attaching-containers-to-cni-networks)
  - [Accessing container network namespace](#accessing-container-network-namespace)
  - [Passing network labels and port-mapping information to CNI plugins](#passing-network-labels-and-port-mapping-information-to-cni-plugins)
- [Networking Recipes](#networking-recipes)
  - [A bridge network](#a-bridge-network)
  - [A port-mapper plugin for CNI networks](#a-port-mapper-plugin)
  - [A Calico network](#a-calico-network)
  - [A Cilium network](#a-cilium-network)
  - [A Weave network](#a-weave-network)


### <a name="motivation"></a>Motivation

Having a separate network namespace for each container is attractive
for orchestration engines such as Mesos, since it provides containers
with network isolation and allows users to operate on containers as if
they were operating on an end-host.  Without network isolation users
have to deal with managing network resources such as TCP/UDP ports on
an end host, complicating the design of their application.

The challenge is in implementing the ability in the orchestration
engine to communicate with the underlying network in order to
configure IP connectivity to the container.  This problem arises due
to the diversity in terms of the choices of IPAM (IP address
management system) and networking technologies available for enabling
IP connectivity. To solve this problem we would need to adopt a driver
based network orchestration model, where the `MesosContainerizer` can
offload the business intelligence of configuring IP connectivity to a
container, to network specific drivers.

The **[Container Network Interface
(CNI)](https://github.com/containernetworking/cni)** is a
specification proposed by CoreOS that provides such a driver based
model. The specification defines a JSON schema that defines the inputs
and outputs expected of a CNI plugin (network driver). The
specification also provides a clear separation of concerns for the
container run time and the CNI plugin. As per the specification the
container run time is expected to configure the namespace for the
container, a unique identifier for the container (container ID), and a
JSON formatted input to the plugin that defines the configuration
parameters for a given network. The responsibility of the plugin is to
create a *veth* pair and attach one of the *veth* pairs to the network
namespace of the container, and the other end to a network understood
by the plugin.  The CNI specification also allows for multiple
networks to exist simultaneously, with each network represented by a
canonical name, and associated with a unique CNI configuration. There
are already CNI plugins for a variety of networks such as bridge,
ipvlan, macvlan, Calico, Weave and Flannel.

Thus, introducing support for CNI in Mesos through the `network/cni`
isolator provides Mesos with tremendous flexibility to orchestrate
containers on a wide variety of network technologies.


### <a name="usage"></a>Usage

The `network/cni` isolator is enabled by default.  However, to use the
isolator there are certain actions required by the operator and the
frameworks. In this section we specify the steps required by the
operator to configure CNI networks on Mesos and the steps required by
frameworks to attach containers to a CNI network.

#### <a name="configuring-cni-networks"></a>Configuring CNI networks

In order to configure the `network/cni` isolator the operator
specifies two flags at Agent startup as follows:

```{.console}
sudo mesos-slave --master=<master IP> --ip=<Agent IP>
  --work_dir=/var/lib/mesos
  --network_cni_config_dir=<location of CNI configs>
  --network_cni_plugins_dir=<search path for CNI plugins>
```

Note that the `network/cni` isolator learns all the available networks
by looking at the CNI configuration in the `--network_cni_config_dir`
at startup. This implies that if a new CNI network needs to be added
after Agent startup, the Agent needs to be restarted. The
`network/cni` isolator has been designed with `recover` capabilities
and hence restarting the Agent (and therefore the `network/cni`
isolator) will not affect container orchestration.

Optionally, the operator could specify the
`--network_cni_root_dir_persist` flag. This flag would allow
`network/cni` isolator to persist the network related information
across reboot and allow `network/cni` isolator to carry out network
cleanup post reboot. This is useful for the CNI networks that depend
on the isolator to clean their network state.

#### <a name="adding-modifying-deleting"></a>Adding/Deleting/Modifying CNI networks

The `network/cni` isolator learns about all the CNI networks by
reading the CNI configuration specified in `--network_cni_config_dir`.
Hence, if the operator wants to add a CNI network, the corresponding
configuration needs to be added to `--network_cni_config_dir`.

While the `network/cni` isolator learns the CNI networks by reading
the CNI configuration files in `--network_cni_config_dir`, it does not
keep an in-memory copy of the CNI configurations. The `network/cni`
isolator only stores a mapping of the CNI network names to the
corresponding CNI configuration files. Whenever the `network/cni`
isolator needs to attach a container to a CNI network it reads the
corresponding configuration from the disk and invokes the appropriate
plugin with the specified JSON configuration. Though the `network/cni`
isolator does not keep an in-memory copy of the JSON configuration, it
checkpoints the CNI configuration used to launch a container.
Checkpointing the CNI configuration protects the resources, associated
with the container, by freeing them correctly when the container is
destroyed, even if the CNI configuration is deleted.

The fact that the `network/cni` isolator always reads the CNI
configurations from the disk allows the operator to dynamically add,
modify and delete CNI configurations without the need to restart the
agent. Whenever the operator modifies an existing CNI configuration,
the agent will pick up this new CNI configuration when the next
container is launched on that specific CNI network. Similarly when the
operator deletes a CNI network the `network/cni` isolator will
"unlearn" the CNI network (since it will have a reference to this CNI
network when it started) in case a framework tries to launch a
container on the deleted CNI network.

#### <a name="attaching-containers-to-cni-networks"></a>Attaching containers to CNI networks

Frameworks can specify the CNI network to which they want their
containers to be attached by setting the `name` field in the
`NetworkInfo` protobuf. The `name` field was introduced in the
`NetworkInfo` protobuf as part of
[MESOS-4758](https://issues.apache.org/jira/browse/MESOS-4758).  Also,
by specifying multiple instances of the `NetworkInfo` protobuf with
different `name` in each of the protobuf, the `MesosContainerizer`
will attach the container to all the different CNI networks specified.

The **default behavior** for containers is to join the ``host
network``, i.e., if the framework *does not* specify a `name` in the
`NetworkInfo` protobuf, the `network/cni` isolator will be a no-op for
that container and will not associate a new network namespace with the
container. This would effectively make the container use the host
network namespace, ``attaching`` it to the host network.


```
**NOTE**: While specifying multiple `NetworkInfo` protobuf allows a
container to be attached to different CNI networks, if one of the
`NetworkInfo` protobuf is without the `name` field the `network/cni`
isolator simply "skips" the protobuf, attaching the container to all
the specified CNI networks except the `host network`.  To attach a
container to the host network as well as other CNI networks you
will need to attach the container to a CNI network (such as
bridge/macvlan) that, in turn, is attached to the host network.
```

#### <a name="network-labels-and-port-mapping">Passing network labels and port-mapping information to CNI plugins</a>

When invoking CNI plugins (e.g., with command ADD), the isolator will
pass on some Mesos meta-data to the plugins by specifying the `args`
field in the [network configuration
JSON](https://github.com/containernetworking/cni/blob/master/SPEC.md#network-configuration)
according to the CNI spec. Currently, the isolator only passes on
`NetworkInfo` of the corresponding network to the plugin. This is
simply the JSON representation of the `NetworkInfo` protobuf. For
instance:

```{.json}
{
  "name" : "mynet",
  "type" : "bridge",
  "args" : {
    "org.apache.mesos" : {
      "network_info" : {
        "name" : "mynet",
        "labels" : {
          "labels" : [
            { "key" : "app", "value" : "myapp" },
            { "key" : "env", "value" : "prod" }
          ]
        },
        "port_mappings" : [
          { "host_port" : 8080, "container_port" : 80 },
          { "host_port" : 8081, "container_port" : 443 }
        ]
      }
    }
  }
}
```

It is important to note that `labels` or `port_mappings` within the
`NetworkInfo` is set by frameworks launching the container, and the
isolator passses on this information to the CNI plugins. As per the
spec, it is the prerogative of the CNI plugins to use this meta-data
information as they see fit while attaching/detaching containers to a CNI
network. E.g., CNI plugins could use `labels` to enforce domain
specific policies, or `port_mappings` to implement NAT rules.

#### <a name="accessing-container-network-namespace"></a>Accessing container network namespace

The `network/cni` isolator allocates a network namespace to a
container when it needs to attach the container to a CNI network. The
network namespace is checkpointed on the host file system and can be
useful to debug network connectivity to the network namespace. For a
given container the `network/cni` isolator checkpoints its network
namespace at:

```{.console}
/var/run/mesos/isolators/network/cni/<container ID>/ns
```

The network namespace can be used with the `ip` command from the
[iproute2](http://man7.org/linux/man-pages/man8/ip-netns.8.html)
package by creating a symbolic link to the network namespace. Assuming
the container ID is `5baff64c-d028-47ba-864e-a5ee679fc069` you can
create the symlink as follows:

```{.console}
ln -s /var/run/mesos/isolators/network/cni/5baff64c-d028-47ba-8ff64c64e-a5ee679fc069/ns /var/run/netns/5baff64c
```

Now we can use the network namespace identifier `5baff64c` to run
commands in the new network name space using the
[iproute2](http://man7.org/linux/man-pages/man8/ip-netns.8.html) package.
E.g. you can view all the links in the container network namespace by
running the command:

```{.console}
ip netns exec 5baff64c ip link
```

Similarly you can view the container's route table by running:

```{.console}
ip netns exec 5baff64c ip route show
```

*NOTE*: Once
[MESOS-5278](https://issues.apache.org/jira/browse/MESOS-5278) is
completed, executing commands within the container network namespace
would be simplified and we will no longer have a dependency on the
`iproute2` package to debug Mesos container networking.


### <a name="networking-recipes"></a>Networking Recipes

This section presents examples for launching containers on different
CNI networks. For each of the examples the assumption is that the CNI
configurations are present at `/var/lib/mesos/cni/config`, and the
plugins are present at `/var/lib/mesos/cni/plugins`. The Agents
therefore need to be started with the following command:

```{.console}
sudo mesos-slave --master=<master IP> --ip=<Agent IP>
--work_dir=/var/lib/mesos
--network_cni_config_dir=/var/lib/mesos/cni/config
--network_cni_plugins_dir=/var/lib/mesos/cni/plugins
--isolation=filesystem/linux,docker/runtime
--image_providers=docker
```

Apart from the CNI configuration parameters, we are also starting the
Agent with the ability to launch docker images on
`MesosContainerizer`. We enable this ability in the
`MesosContainerizer` by enabling the `filesystem/linux` and
`docker/runtime` isolator and setting the image provider to
`docker`.

To present an example of a framework launching containers on a
specific CNI network, the `mesos-execute` CLI framework has been
modified to take a `--networks` flag which will allow this example
framework to launch containers on the specified network. You can find
the `mesos-execute` framework in your Mesos installation directory at
`<mesos installation>/bin/mesos-execute`.

#### <a name="a-bridge-network"></a>A bridge network

The
[bridge](https://github.com/containernetworking/cni/blob/master/Documentation/bridge.md)
plugin attaches containers to a Linux bridge. Linux bridges could be
configured to attach to VLANs and VxLAN allowing containers to be
plugged into existing layer 2 networks. We present an example below,
where the CNI configuration instructs the `MesosContainerizer` to
invoke a bridge plugin to connect a container to a Linux bridge. The
configuration also instructs the bridge plugin to assign an IP address
to the container by invoking a
[host-local](https://github.com/containernetworking/cni/blob/master/Documentation/host-local.md)
IPAM.

First, build the CNI plugin according to the instructions in the [CNI
repository](https://github.com/containernetworking/cni) then copy the
bridge binary to the plugins directory on each agent.

Next, create the configuration file and copy this to the CNI
configuration directory on each agent.

```{.json}
{
"name": "cni-test",
"type": "bridge",
"bridge": "mesos-cni0",
"isGateway": true,
"ipMasq": true,
"ipam": {
    "type": "host-local",
    "subnet": "192.168.0.0/16",
    "routes": [
    { "dst":
      "0.0.0.0/0" }
    ]
  }
}
```

The CNI configuration tells the bridge plugin to attach the
container to a bridge called `mesos-cni0`. If the bridge does not
exist the bridge plugin will create one.

It is important to note the `routes` section in the `ipam` dictionary.
For Mesos, the `executors` launched as containers need to register
with the Agent in order for a task to be successfully launched.
Hence, it is imperative that the Agent IP is reachable from the
container IP and vice versa. In this specific instance we specified a
default route for the container, allowing containers to reach any
network that will be routeable by the gateway, which for this CNI
configuration is the bridge itself.

Another interesting attribute in the CNI configuration is the `ipMasq`
option. Setting this to true will install an `iptable` rule in the
host network namespace that would SNAT all traffic originating from
the container and egressing the Agent. This allows containers to talk
to the outside world even when they are in an address space that is
not routeable from outside the agent.

Below we give an example of launching a `Ubuntu` container and
attaching it to the `mesos-cni0` bridge. You can launch the `Ubuntu`
container using the `mesos-execute` framework as follows:

```{.console}
sudo mesos-execute --command=/bin/bash
  --docker_image=ubuntu:latest --master=<master IP>:5050 --name=ubuntu
  --networks=cni-test --no-shell
```

The above command would pull the `Ubuntu` image from the docker hub
and launch it using the `MesosContainerizer` and attach it to the
`mesos-cni0` bridge.

You can verify the network settings of the `Ubuntu` container by
creating a symlink to the network namespace and running the `ip`
command as describe in the section "[Accessing container network
namespace](#accessing-container-network-namespace)".

Assuming we created a reference for the network namespace in
`/var/run/netns/5baff64c` . The output of the IP address and route table
in the container network namespace would be as follows:

```{.console}
$ sudo ip netns exec 5baff64c ip addr show
1: lo: <LOOPBACK> mtu 65536 qdisc noop state DOWN group default
    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
3: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc noqueue state UP group default
    link/ether 8a:2c:f9:41:0a:54 brd ff:ff:ff:ff:ff:ff
    inet 192.168.0.2/16 scope global eth0
       valid_lft forever preferred_lft forever
    inet6 fe80::882c:f9ff:fe41:a54/64 scope link
       valid_lft forever preferred_lft forever

$ sudo ip netns exec 5baff64c ip route show
default via 192.168.0.1 dev eth0
192.168.0.0/16 dev eth0  proto kernel  scope link  src 192.168.0.2
```

#### <a name="a-port-mapper-plugin">A port-mapper plugin for CNI networks</a>

For private, isolated, networks such as a bridge network where the IP
address of a container is not routeable from outside the host it
becomes imperative to provide containers with DNAT capabilities so
that services running on the container can be exposed outside the host
on which the container is running.

Unfortunately, there is no CNI plugin available in the
[containernetworking/cni](https://github.com/containernetworking/cni)
repository that provides port-mapping functionality.
Hence, we have developed a port-mapper CNI plugin that resides
within the Mesos code base called the `mesos-cni-port-mapper`. The
`mesos-cni-port-mapper` is designed to work with any other CNI plugin
that requires DNAT capabilities. One of the most obvious being the
`bridge` CNI plugin.

We explain the operational semantics of the `mesos-cni-port-mapper`
plugin by taking an example CNI configuration that allows the
`mesos-cni-port-mapper` to provide DNAT functionality to the `bridge`
plugin.

```
{
  "name" : "port-mapper-test",
  "type" : "mesos-cni-port-mapper",
  "excludeDevices" : ["mesos-cni0"],
  "chain": "MESOS-TEST-PORT-MAPPER",
  "delegate": {
      "type": "bridge",
      "bridge": "mesos-cni0",
      "isGateway": true,
      "ipMasq": true,
      "ipam": {
        "type": "host-local",
        "subnet": "192.168.0.0/16",
        "routes": [
        { "dst":
          "0.0.0.0/0" }
        ]
      }
  }
}
```

For the CNI configuration above, apart from the parameters that the
`mesos-cni-port-mapper` plugin accepts, the important point to note in
the CNI configuration of the plugin is the "delegate" field. The
"delegate" field allows the `mesos-cni-port-mapper` to wrap the CNI
configuration of any other CNI plugin, and allows the plugin to
provide DNAT capabilities to any CNI network. In this specific case
the `mesos-cni-port-mapper` is providing DNAT capabilities to
containers running on the bridge network `mesos-cni0`. The parameters
that the `mesos-cni-port-mapper` accepts are listed below:

* ***name*** : Name of the CNI network.
* ***type*** : Name of the port-mapper CNI plugin.
* ***chain*** : The chain in which the iptables DNAT rule
will be added in the NAT table. This allows the operator to group
DNAT rules for a given CNI network under its own chain, allowing for
better management of the iptables rules.
* ***excludeDevices***: These are a list of ingress devices on which the
DNAT rule should not be applied.
* ***delegate*** : This is a JSON dict that holds the CNI JSON configuration
of a CNI plugin that the port-mapper plugin is expected to invoke.

The `mesos-cni-port-mapper` relies heavily on `iptables` to provide
the DNAT capabilities to a CNI network. In order for the port-mapper
plugin to function properly we have certain minimum
**requirements for iptables** as listed below:

* ***iptables 1.4.20 or higher***: This because we need to use the -w option
of iptables in order to allow atomic writes to iptables.
* ***Require the xt_comments module of iptables***: We use the comments
module to tag iptables rules belonging to a container. These tags are used as a
key while deleting iptables rules when the specific container is deleted.

Finally, while the CNI configuration of the port-mapper plugin tells
the plugin as to how and where to install the `iptables` rules, and which
CNI plugin to "delegate" the attachment/detachment of the container, the
port-mapping information itself is learned by looking at the
`NetworkInfo` set in the `args` field of the CNI configuration passed
by Mesos to the port-mapper plugin. Please refer to the "[Passing
network labels and port-mapping information to CNI
plugins](#passing-network-labels-and-port-mapping-information-to-cni-plugins)" section for more details.

#### <a name="a-calico-network">A Calico network</a>

[Calico](https://projectcalico.org/) provides 3rd-party CNI plugin
that works out-of-the-box with Mesos CNI.

Calico takes a pure Layer-3 approach to networking, allocating a
unique, routable IP address to each Meso task. Task routes are
distributed by a BGP vRouter run on each Agent, which leverages the
existing Linux kernel forwarding engine without needing tunnels, NAT,
or overlays. Additionally, Calico supports rich and flexible network
policy which it enforces using bookended ACLs on each compute node to
provide tenant isolation, security groups, and external reachability
constraints.

For information on setting up and using Calico-CNI, see [Calico's
guide on integerating with
Mesos](https://docs.projectcalico.org/v2.6/getting-started/mesos/).

#### <a name="a-cilium-network">A Cilium network</a>

[Cilium](https://www.cilium.io) provides a CNI plugin that works with Mesos.

Cilium brings HTTP-aware network security filtering to Linux container
frameworks. Using a new Linux kernel technology called BPF, Cilium
provides a simple and efficient way to define and enforce both
network-layer and HTTP-layer security policies.

For more information on using Cilium with Mesos, check out the
[Getting Started Using Mesos Guide](http://docs.cilium.io/try-mesos).

#### <a name="a-weave-network">A Weave network</a>

[Weave](https://weave.works) provides a CNI implementation that works
out-of-the-box with Mesos.

Weave provides hassle free configuration by assigning an
ip-per-container and providing a fast DNS on each node. Weave is fast,
by automatically choosing the fastest path between hosts. Multicast
addressing and routing is fully supported. It has built in NAT
traversal and encryption and continues to work even during a network
partition.  Finally, Multi-cloud deployments are easy to setup and
maintain, even when there are multiple hops.

For more information on setting up and using Weave CNI, see [Weave's
CNI
documentation](https://www.weave.works/docs/net/latest/cni-plugin/)
