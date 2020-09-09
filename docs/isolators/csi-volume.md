---
title: Apache Mesos - CSI Volume Support in Mesos Containerizer
layout: documentation
---

# Pre-provisioned CSI Volume Support in Mesos Containerizer

Mesos 1.11.0 adds pre-provisioned CSI volume support to the
[MesosContainerizer](../mesos-containerizer.md) (a.k.a., the universal
containerizer) by introducing the new `volume/csi` isolator.

This document describes the motivation and the configuration steps for enabling
the `volume/csi` isolator, and required framework changes.

## Table of Contents
- [Motivation](#motivation)
- [How does it work?](#how-does-it-work)
- [Configuration](#configuration)
  - [Pre-conditions](#pre-conditions)
  - [Configuring CSI Volume Isolator](#configure-csi-volume-isolator)
  - [Enabling frameworks to use CSI volumes](#enable-frameworks)
    - [Volume Protobuf](#volume-protobuf)
    - [Example](#example)

## <a name="motivation"></a>Motivation

[Container Storage Interface](https://github.com/container-storage-interface/spec)
(CSI) is a specification that defines a common set of APIs for all interactions
between the storage vendors and the container orchestration platforms. Building
CSI support allows Mesos to make use of the quickly-growing CSI ecosystem.

We already have a [solution](../csi.md) to support CSI introduced in the Mesos
1.5.0 release, but that solution has a limitation: it requires CSI plugins to
implement the [ListVolumes](https://github.com/container-storage-interface/spec/blob/v1.3.0/spec.md#listvolumes)
and [GetCapacity](https://github.com/container-storage-interface/spec/blob/v1.3.0/spec.md#getcapacity)
APIs so that the external storage can be modeled as Mesos raw disk resources and
then offered to frameworks. However there are a lot of 3rd party CSI plugins the
do not implement those two APIs.

Mesos 1.11.0 provides a more generic way to support 3rd party CSI plugins so
that Mesos can work with broader external storage ecosystem and we will benefit
from continued development of the community CSI plugins.

## <a name="how-does-it-work"></a>How does it work?

The `volume/csi` isolator interacts with CSI plugins via the plugin's gRPC
endpoint.

When a new task with CSI volumes is launched, the `volume/csi` isolator will
call the CSI plugin to publish the specified CSI volumes onto the agent host
and then mount them onto the task container. When the task terminates, the
`volume/csi` isolator will call the CSI plugin to unpublish the specified CSI
volumes.

Currently the `volume/csi` isolator will only call the CSI plugin's [node service](https://github.com/container-storage-interface/spec/blob/v1.3.0/spec.md#node-service-rpc) but not [controller service](https://github.com/container-storage-interface/spec/blob/v1.3.0/spec.md#controller-service-rpc), that means:

- We only support pre-provisioned CSI volume but not dynamic CSI volumes
  provisioning, so operators need to create the CSI volumes explicitly and
  provide the volume info (e.g. volume ID, context, etc.) to frameworks so
  that frameworks can use the volumes in their tasks.

- We do not support the CSI volumes that require the controller service to
  publish to a node ([ControllerPublishVolume](https://github.com/container-storage-interface/spec/blob/v1.3.0/spec.md#controllerpublishvolume)) prior to the node service publishing on the node
  ([NodePublishVolume](https://github.com/container-storage-interface/spec/blob/v1.3.0/spec.md#nodepublishvolume)).

## <a name="configuration"></a>Configuration

To use the `volume/csi` isolator, there are certain actions required by
operators and framework developers. In this section we list the steps
required by the operator to configure the `volume/csi` isolator and the steps
required by framework developers to specify CSI volumes in their tasks.

### <a name="pre-conditions"></a>Pre-conditions

- Explicitly create the CSI volumes that are going to be accessed by Mesos
  tasks. For some CSI plugins (e.g. [NFS](https://github.com/kubernetes-csi/csi-driver-nfs)),
  they do not implement the [CreateVolume](https://github.com/container-storage-interface/spec/blob/v1.3.0/spec.md#createvolume)
  API, so operators do not need to create the volume explicitly in this case.

### <a name="configure-csi-volume-isolator"></a>Configuring the CSI Volume Isolator

In order to configure the `volume/csi` isolator, the operator needs to
configure the `--isolation` and `--csi_plugin_config_dir` flags at agent
startup as follows:

```{.console}
  sudo mesos-agent \
    --master=<master-IP:master-port> \
    --work_dir=/var/lib/mesos \
    --isolation=filesystem/linux,volume/csi \
    --csi_plugin_config_dir=<directory that contains CSI plugin configuration files>
```

The `volume/csi` isolator must be specified in the `--isolation` flag at agent
startup; the `volume/csi` isolator has a dependency on the `filesystem/linux`
isolator.

The operator needs to put the CSI plugin configuration files under the directory
specified via the agent flag `--csi_plugin_config_dir`. Each file in this
directory should contain a JSON object representing a `CSIPluginInfo` object
which can be either a managed CSI plugin (i.e. the plugin launched by Mesos as
a standalone container) or an unmanaged CSI plugin (i.e. the plugin launched
outside of Mesos).

```{.proto}
message CSIPluginInfo {
  required string type = 1;
  optional string name = 2 [default = "default"];

  // A list of container configurations to run managed CSI plugin.
  repeated CSIPluginContainerInfo containers = 3;

  // The service endpoints of the unmanaged CSI plugin.
  repeated CSIPluginEndpoint endpoints = 4;

  optional string target_path_root = 5;
  optional bool target_path_exists = 6;
}

message CSIPluginContainerInfo {
  enum Service {
    UNKNOWN = 0;
    CONTROLLER_SERVICE = 1;
    NODE_SERVICE = 2;
  }

  repeated Service services = 1;
  optional CommandInfo command = 2;
  repeated Resource resources = 3;
  optional ContainerInfo container = 4;
}

message CSIPluginEndpoint {
  required CSIPluginContainerInfo.Service csi_service = 1;
  required string endpoint = 2;
}
```

Example of managed CSI plugin:
```{.json}
{
  "type": "org.apache.mesos.csi.managed-plugin",
  "containers": [
    {
      "services": [
        "NODE_SERVICE"
      ],
      "command": {
        "value": "<path-to-managed-plugin> --endpoint=$CSI_ENDPOINT"
      },
      "resources": [
        {"name": "cpus", "type": "SCALAR", "scalar": {"value": 0.1}},
        {"name": "mem", "type": "SCALAR", "scalar": {"value": 1024}}
      ]
    }
  ]
}
```

Example of unmanaged CSI plugin:
```{.json}
{
  "type": "org.apache.mesos.csi.unmanaged-plugin",
  "endpoints": [
    {
      "csi_service": "NODE_SERVICE",
      "endpoint": "/var/lib/unmanaged-plugin/csi.sock"
    }
  ],
  "target_path_root": "/mnt/unmanaged-plugin"
}
```

### <a name="enable-frameworks"></a>Enabling frameworks to use CSI volumes

#### <a name="volume-protobuf"></a>Volume Protobuf

The `Volume` protobuf message has been updated to support CSI volumes.

```{.proto}
message Volume {
  ...
  required Mode mode = 3;
  required string container_path = 1;

  message Source {
    enum Type {
      UNKNOWN = 0;
      ...
      CSI_VOLUME = 5;
    }

    message CSIVolume {
      required string plugin_name = 1;

      message VolumeCapability {
        message BlockVolume {
        }

        message MountVolume {
          optional string fs_type = 1;
          repeated string mount_flags = 2;
        }

        message AccessMode {
          enum Mode {
            UNKNOWN = 0;
            SINGLE_NODE_WRITER = 1;
            SINGLE_NODE_READER_ONLY = 2;
            MULTI_NODE_READER_ONLY = 3;
            MULTI_NODE_SINGLE_WRITER = 4;
            MULTI_NODE_MULTI_WRITER = 5;
          }

          required Mode mode = 1;
        }

        oneof access_type {
          BlockVolume block = 1;
          MountVolume mount = 2;
        }

        required AccessMode access_mode = 3;
      }

      // Specifies the parameters used to stage/publish a pre-provisioned volume
      // on an agent host.
      message StaticProvisioning {
        required string volume_id = 1;
        required VolumeCapability volume_capability = 2;
        optional bool readonly = 3;
        map<string, Secret> node_stage_secrets = 4;
        map<string, Secret> node_publish_secrets = 5;
        map<string, string> volume_context = 6;
      }

      optional StaticProvisioning static_provisioning = 2;
    }

    optional Type type = 1;
    ...
    optional CSIVolume csi_volume = 6;
  }

  optional Source source = 5;
}
```

When requesting a CSI volume for a container, the framework developer needs to
set `Volume` for the container, which includes `mode`, `container_path` and
`source` fields.

The `source` field specifies where the volume comes from. Framework developers
need to set the `type` field to `CSI_VOLUME` and specify the `csi_volume` field.

The `csi_volume` field specifies the information of the CSI volume. Framework
developers need to set the `plugin_name` field to the `type` field of one of the
CSI plugin configuration files in the directory specified via the agent flag
`--csi_plugin_config_dir`, and specify the `static_provisioning` field according
to the information of the pre-provisioned volume. The fields in `static_provisioning`
map directly onto the fields in the CSI calls [NodeStageVolume](https://github.com/container-storage-interface/spec/blob/v1.3.0/spec.md#nodestagevolume)
and [NodePublishVolume](https://github.com/container-storage-interface/spec/blob/v1.3.0/spec.md#nodepublishvolume),
please find more detailed descriptions about those fields in the CSI spec.

How to specify `container_path`:

1. If you are launching a task without a container image and `container_path`
   is an absolute path, you need to make sure the absolute path exists on your
   host root file system as the container shares the host root file system;
   otherwise, the task will fail.

2. For other cases like launching a task without a container image and with a
   relative `container_path`, or launching a task with a container image and an
   absolute or relative `container_path`, the `volume/csi` isolator will help
   create the `container_path` as the mount point.

The following table summarizes the above rules for `container_path`:

<table class="table table-striped">
  <tr>
    <th></th>
    <th>Task with rootfs</th>
    <th>Task without rootfs</th>
  </tr>
  <tr>
    <td>Absolute container_path</td>
    <td>No need to exist</td>
    <td>Must exist</td>
  </tr>
  <tr>
    <td>Relative container_path</td>
    <td>No need to exist</td>
    <td>No need to exist</td>
  </tr>
</table>

#### <a name="example"></a>Example

Launch a task with a CSI volume managed by NFS CSI plugin:

   ```{.json}
   TaskInfo {
     ...
     "command" : {
       "value": "echo test > volume/file"
     },
     "container" : {
       "type": "MESOS",
       "volumes" : [
         {
           "container_path" : "volume",
           "mode" : "RW",
           "source": {
             "type": "CSI_VOLUME",
             "csi_volume": {
               "plugin_name": "nfs.csi.k8s.io",
               "static_provisioning": {
                 "volume_id": "foo",
                 "volume_capability": {
                   "mount": {},
                   "access_mode": {
                     "mode": "MULTI_NODE_MULTI_WRITER"
                   }
                 },
                 "volume_context": {
                   "server": "192.168.1.100",
                   "share": "/mnt/data"
                 }
               }
             }
           }
         }
       ]
     }
   }
   ```

**NOTE**: To make the above example work, an NFS server (`192.168.1.100`) needs to
be setup to export the directory `/mnt/data`.
