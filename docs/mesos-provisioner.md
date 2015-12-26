# Mesos Image Provisioner

## Motivation

There are multiple container specifications, notably Docker, Appc (rkt), and recently OCP (oci). Most of the container specifications, to varying degrees, conflate image format specification with other components of a container, including execution and resource isolation, both in specification and implementation.

The goal of Mesos Image Provisioner is to extend the MesosContainerizer to support provisioning container filesystems from different image formats while composing with the existing Isolators for resource isolation.

Mesos image provisioner allows Mesos containers to be created and managed through Mesos containerizer to have a root filesystem provisioned with image bundles with common image specification formats such as AppContainer and Docker.

## Glossary

Layer: A layer is typically a filesystem changeset.

Image: An image in this documentation refers to a container filesystem image. A filesystem image typically contains one or more layers.

## Framework API

We introduced a new protobuf message `Image` that describes a container filesystem image.

message Image {
  enum Type {
    APPC = 1;
    DOCKER = 2;
    // More Image types.
  }

  message Appc {
    // Appc configurations.
  }

  message Docker {
    // Docker configurations
  }

  required Type type = 1;

  // Only one of the following image messages should be set to match
  // the type.
  optional Appc appc = 2;
  optional Docker docker = 3;
}

This `Image` message type contains the type of image specification and the corresponding configurations for that type.

The image type is currently supported to be specified in both ContainerInfo and Volume.

When ContainerInfo image is configured, the Mesos image provisioner provides a root filesystem to the task. On the other hand, when volume(s) are configured with an image,
that volume will be mounted with the image filesystem configured.

Image can both be configured in a Volume and in ContainerInfo at the same time.

## Setup and Agent Flags

To run the agent to enable Mesos containerizer, you must launch the agent with mesos as a containerizer option, which is the default containerizer for the mesos agent. It also has to enable filesystem/linux as part of the enabled isolators. The supported image providers can also be configured through agent flags, as well as the supported image provider backend.

Mesos agent also needs to be running under linux with root permissions.

* Example: `mesos-slave --containerizers=mesos --image_providers=appc,docker --image_provisioner_backend=copy --isolation=filesystem/linux`

Each Image provider can have additional configurations that can be set.

## Mesos Image Provisioner

Mesos provisioner forwards the container image request to the corresponding Image Provider to provision the image changesets (layers), and then ask the configured Backend to provision a root filesystem from the layers.

## Image providers

An image provider is a provider that understands how to discover, download and unpack a particular image format.
Mesos agent supports multiple image providers and the `image_providers` agent flag allows operators to choose which ones to support.

### Appc

https://github.com/appc/spec/blob/master/spec/aci.md

TODO(tnachen): Add Appc information.

### Docker

https://github.com/docker/docker/blob/master/image/spec/v1.md

Docker image provider supports two different methods of finding and pulling images: local and registry. The `docker_registry` agent flag allows the slave to choose between these methods based on the URL scheme.

By specifying a URL in `docker_registry` agent flag pointing to a local directory (file:///tmp/mesos/images/docker), the provisioner will use the Local puller to find docker images based on image name and tag in the host filesystem.

If the URL configured in `docker_registry` isn't pointing to a local directory, it will be assumed to be a registry referring to a Docker registry. The Registry puller will find and download images by contacting Docker registry with the configured URL when no custom registry is specified.

Note that to run the Registry puller Mesos agent must be running with SSL enabled.

## Image provisioner backend

A provisioner backend takes the layers that the image provider provided and build a root filesystem for a container or volume.

### Copy

The Copy backend simply copies all the layers into a target root directory to create a root filesystem.

### Bind

This is a specialized backend that may be useful for deployments using large (multi-GB) single-layer images *and* where more recent kernel features such as overlayfs are not available (overlayfs-based
backend tracked by MESOS-2971). For small images (10's to 100's of MB) the copy backend may be sufficient. Bind backend is faster than Copy as it requires nearly zero IO.

The Bind backend currently has these two limitations:
1) BindBackend supports only a single layer. Multi-layer images will fail to provision and the container will fail to launch!
2) The filesystem is read-only because all containers using this image share the source. Select writable areas can be achieved by mounting read-write volumes to places like /tmp, /var/tmp, /home, etc. using the ContainerInfo. These can be relative to the executor work directory. Since the filesystem is read-only, '--sandbox_directory' must already exist within the filesystem because the filesystem isolator is unable to create it!

## Internals

The design doc is available [here](https://docs.google.com/document/d/1Fx5TS0LytV7u5MZExQS0-g-gScX2yKCKQg9UPFzhp6U).

## Related Docs
For more information on the Mesos containerizer filesystem, namespace, and isolator features, visit [here](https://github.com/apache/mesos/blob/master/docs/containerizer.md).
For more information on launching Docker containers through the Docker containerizer, visit [here](https://github.com/apache/mesos/blob/master/docs/docker-containerizer.md).