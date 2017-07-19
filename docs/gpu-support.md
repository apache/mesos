---
title: Apache Mesos - Nvidia GPU Support
layout: documentation
---

# Nvidia GPU Support

Mesos 1.0.0 added first-class support for Nvidia GPUs.
The minimum required Nvidia driver version is `340.29`.

## Overview
Getting up and running with GPU support in Mesos is fairly
straightforward once you know the steps necessary to make it work as
expected. On one side, this includes setting the necessary agent flags
to enumerate GPUs and advertise them to the Mesos master. On the other
side, this includes setting the proper framework capabilities so that
the Mesos master will actually include GPUs in the resource offers it
sends to a framework. So long as all of these constraints are met,
accepting offers that contain GPUs and launching tasks that consume
them should be just as straightforward as launching a traditional task
that only consumes CPUs, memory, and disk.

Mesos exposes GPUs as a simple `SCALAR` resource in the same
way it always has for CPUs, memory, and disk. That is, a resource
offer such as the following is now possible:

    cpus:8; mem:1024; disk:65536; gpus:4;

However, unlike CPUs, memory, and disk, *only* whole numbers of GPUs
can be selected. If a fractional amount is selected, launching the
task will result in a `TASK_ERROR`.

At the time of this writing, Nvidia GPU support is only available for
tasks launched through the Mesos containerizer (i.e., no support exists
for launching GPU capable tasks through the Docker containerizer).
That said, the Mesos containerizer now supports running docker
images natively, so this limitation should not affect most users.

Moreover, we mimic the support provided by [nvidia-docker](
https://github.com/NVIDIA/nvidia-docker/wiki/NVIDIA-driver) to
automatically mount the proper Nvidia drivers and tools directly into
your docker container. This means you can easily test your GPU-enabled
docker containers locally and deploy them to Mesos with the assurance
that they will work without modification.

In the following sections we walk through all of the flags and
framework capabilities necessary to enable Nvidia GPU support in
Mesos. We then show an example of setting up and running an example
test cluster that launches tasks both with and without docker
containers. Finally, we conclude with a step-by-step guide of how to
install any necessary Nvidia GPU drivers on your machine.

## Agent Flags
The following isolation flags are required to enable Nvidia GPU
support on an agent.

    --isolation="filesystem/linux,cgroups/devices,gpu/nvidia"

The `filesystem/linux` flag tells the agent to use Linux-specific
commands to prepare the root filesystem and volumes (e.g., persistent
volumes) for containers that require them. Specifically, it relies on
Linux mount namespaces to prevent the mounts of a container from being
propagated to the host mount table. In the case of GPUs, we require
this flag to properly mount certain Nvidia binaries (e.g.,
`nvidia-smi`) and libraries (e.g., `libnvidia-ml.so`) into a container
when necessary.

The `cgroups/devices` flag tells the agent to restrict access to a
specific set of devices for each task that it launches (i.e., a subset
of all devices listed in `/dev`). When used in conjunction with the
`gpu/nvidia` flag, the `cgroups/devices` flag allows us to grant /
revoke access to specific GPUs on a per-task basis.

By default, all GPUs on an agent are automatically discovered and sent
to the Mesos master as part of its resource offer. However, it may
sometimes be necessary to restrict access to only a subset of the GPUs
available on an agent. This is useful, for example, if you want to
exclude a specific GPU device because an unwanted Nvidia graphics card
is listed alongside a more powerful set of GPUs. When this is
required, the following additional agent flags can be used to
accomplish this:

    --nvidia_gpu_devices="<list_of_gpu_ids>"

    --resources="gpus:<num_gpus>"

For the `--nvidia_gpu_devices` flag, you need to provide a comma
separated list of GPUs, as determined by running `nvidia-smi` on the
host where the agent is to be launched ([see
below](#external-dependencies) for instructions on what external
dependencies must be installed on these hosts to run this command).
Example output from running `nvidia-smi` on a machine with four GPUs
can be seen below:

    +------------------------------------------------------+
    | NVIDIA-SMI 352.79     Driver Version: 352.79         |
    |-------------------------------+----------------------+----------------------+
    | GPU  Name        Persistence-M| Bus-Id        Disp.A | Volatile Uncorr. ECC |
    | Fan  Temp  Perf  Pwr:Usage/Cap|         Memory-Usage | GPU-Util  Compute M. |
    |===============================+======================+======================|
    |   0  Tesla M60           Off  | 0000:04:00.0     Off |                    0 |
    | N/A   34C    P0    39W / 150W |     34MiB /  7679MiB |      0%      Default |
    +-------------------------------+----------------------+----------------------+
    |   1  Tesla M60           Off  | 0000:05:00.0     Off |                    0 |
    | N/A   35C    P0    39W / 150W |     34MiB /  7679MiB |      0%      Default |
    +-------------------------------+----------------------+----------------------+
    |   2  Tesla M60           Off  | 0000:83:00.0     Off |                    0 |
    | N/A   38C    P0    40W / 150W |     34MiB /  7679MiB |      0%      Default |
    +-------------------------------+----------------------+----------------------+
    |   3  Tesla M60           Off  | 0000:84:00.0     Off |                    0 |
    | N/A   34C    P0    39W / 150W |     34MiB /  7679MiB |     97%      Default |
    +-------------------------------+----------------------+----------------------+

The GPU `id` to choose can be seen in the far left of each row. Any
subset of these `ids` can be listed in the `--nvidia_gpu_devices`
flag (i.e., all of the following values of this flag are valid):

    --nvidia_gpu_devices="0"
    --nvidia_gpu_devices="0,1"
    --nvidia_gpu_devices="0,1,2"
    --nvidia_gpu_devices="0,1,2,3"
    --nvidia_gpu_devices="0,2,3"
    --nvidia_gpu_devices="3,1"
    etc...

For the `--resources=gpus:<num_gpus>` flag, the value passed to
`<num_gpus>` must equal the number of GPUs listed in
`--nvidia_gpu_devices`. If these numbers do not match, launching the
agent will fail. This can sometimes be a source of confusion, so it
is important to emphasize it here for clarity.

## Framework Capabilities
Once you launch an agent with the flags above, GPU resources will be
advertised to the Mesos master along side all of the traditional
resources such as CPUs, memory, and disk. However, the master will
only forward offers that contain GPUs to frameworks that have
explicitly enabled the `GPU_RESOURCES` framework capability.

The choice to make frameworks explicitly opt-in to this `GPU_RESOURCES`
capability was to keep legacy frameworks from accidentally consuming
non-GPU resources on GPU-capable machines (and thus preventing your GPU
jobs from running). It's not that big a deal if all of your nodes have
GPUs, but in a mixed-node environment, it can be a big problem.

An example of setting this capability in a C++-based framework can be
seen below:

    FrameworkInfo framework;
    framework.add_capabilities()->set_type(
          FrameworkInfo::Capability::GPU_RESOURCES);

    GpuScheduler scheduler;

    driver = new MesosSchedulerDriver(
        &scheduler,
        framework,
        127.0.0.1:5050);

    driver->run();


## Minimal GPU Capable Cluster
In this section we walk through two examples of configuring GPU-capable
clusters and running tasks on them. The first example demonstrates the
minimal setup required to run a command that consumes GPUs on a GPU-capable
agent. The second example demonstrates the setup necessary to
launch a docker container that does the same.

**Note**: Both of these examples assume you have installed the
external dependencies required for Nvidia GPU support on Mesos. Please
see [below](#external-dependencies) for more information.

### Minimal Setup Without Support for Docker Containers
The commands below show a minimal example of bringing up a GPU-capable
Mesos cluster on `localhost` and executing a task on it. The required
agent flags are set as described above, and the `mesos-execute`
command has been told to enable the `GPU_RESOURCES` framework
capability so it can receive offers containing GPU resources.

    $ mesos-master \
          --ip=127.0.0.1 \
          --work_dir=/var/lib/mesos

    $ mesos-agent \
          --master=127.0.0.1:5050 \
          --work_dir=/var/lib/mesos \
          --isolation="cgroups/devices,gpu/nvidia"

    $ mesos-execute \
          --master=127.0.0.1:5050 \
          --name=gpu-test \
          --command="nvidia-smi" \
          --framework_capabilities="GPU_RESOURCES" \
          --resources="gpus:1"

If all goes well, you should see something like the following in the
`stdout` out of your task:

    +------------------------------------------------------+
    | NVIDIA-SMI 352.79     Driver Version: 352.79         |
    |-------------------------------+----------------------+----------------------+
    | GPU  Name        Persistence-M| Bus-Id        Disp.A | Volatile Uncorr. ECC |
    | Fan  Temp  Perf  Pwr:Usage/Cap|         Memory-Usage | GPU-Util  Compute M. |
    |===============================+======================+======================|
    |   0  Tesla M60           Off  | 0000:04:00.0     Off |                    0 |
    | N/A   34C    P0    39W / 150W |     34MiB /  7679MiB |      0%      Default |
    +-------------------------------+----------------------+----------------------+

### Minimal Setup With Support for Docker Containers
The commands below show a minimal example of bringing up a GPU-capable
Mesos cluster on `localhost` and running a docker container on it. The
required agent flags are set as described above, and the
`mesos-execute` command has been told to enable the `GPU_RESOURCES`
framework capability so it can receive offers containing GPU
resources.  Additionally, the required flags to enable support for
docker containers (as described [here](container-image.md)) have been
set up as well.

    $ mesos-master \
          --ip=127.0.0.1 \
          --work_dir=/var/lib/mesos

    $ mesos-agent \
          --master=127.0.0.1:5050 \
          --work_dir=/var/lib/mesos \
          --image_providers=docker \
          --executor_environment_variables="{}" \
          --isolation="docker/runtime,filesystem/linux,cgroups/devices,gpu/nvidia"

    $ mesos-execute \
          --master=127.0.0.1:5050 \
          --name=gpu-test \
          --docker_image=nvidia/cuda \
          --command="nvidia-smi" \
          --framework_capabilities="GPU_RESOURCES" \
          --resources="gpus:1"

If all goes well, you should see something like the following in the
`stdout` out of your task.

    +------------------------------------------------------+
    | NVIDIA-SMI 352.79     Driver Version: 352.79         |
    |-------------------------------+----------------------+----------------------+
    | GPU  Name        Persistence-M| Bus-Id        Disp.A | Volatile Uncorr. ECC |
    | Fan  Temp  Perf  Pwr:Usage/Cap|         Memory-Usage | GPU-Util  Compute M. |
    |===============================+======================+======================|
    |   0  Tesla M60           Off  | 0000:04:00.0     Off |                    0 |
    | N/A   34C    P0    39W / 150W |     34MiB /  7679MiB |      0%      Default |
    +-------------------------------+----------------------+----------------------+

<a name="external-dependencies"></a>
## External Dependencies

Any host running a Mesos agent with Nvidia GPU support **MUST** have a
valid Nvidia kernel driver installed. It is also *highly* recommended to
install the corresponding user-level libraries and tools available as
part of the Nvidia CUDA toolkit. Many jobs that use Nvidia GPUs rely
on CUDA and not including it will severely limit the type of
GPU-aware jobs you can run on Mesos.

**Note:** The minimum supported version of CUDA is `6.5`.

### Installing the Required Tools

The Nvidia kernel driver can be downloaded at the link below. Make
sure to choose the proper model of GPU, operating system, and CUDA
toolkit you plan to install on your host:

    http://www.nvidia.com/Download/index.aspx

Unfortunately, most Linux distributions come preinstalled with an open
source video driver called `Nouveau`. This driver conflicts with the
Nvidia driver we are trying to install. The following guides may prove
useful to help guide you through the process of uninstalling `Nouveau`
before installing the Nvidia driver on CentOS or Ubuntu.

    http://www.dedoimedo.com/computers/centos-7-nvidia.html
    http://www.allaboutlinux.eu/remove-nouveau-and-install-nvidia-driver-in-ubuntu-15-04/

After installing the Nvidia kernel driver, you can follow the
instructions in the link below to install the Nvidia CUDA toolkit:

    http://docs.nvidia.com/cuda/cuda-getting-started-guide-for-linux/

In addition to the steps listed in the link above, it is *highly*
recommended to add CUDA's `lib` directory into your `ldcache` so that
tasks launched by Mesos will know where these libraries exist and link
with them properly.

    sudo bash -c "cat > /etc/ld.so.conf.d/cuda-lib64.conf << EOF
    /usr/local/cuda/lib64
    EOF"

    sudo ldconfig

If you choose **not** to add CUDAs `lib` directory to your `ldcache`,
you **MUST** add it to every task's `LD_LIBRARY_PATH` that requires
it.

**Note:** This is *not* the recommended method. You have been warned.

### Verifying the Installation

Once the kernel driver has been installed, you can make sure
everything is working by trying to run the bundled `nvidia-smi` tool.

    nvidia-smi

You should see output similar to the following:

    Thu Apr 14 11:58:17 2016
    +------------------------------------------------------+
    | NVIDIA-SMI 352.79     Driver Version: 352.79         |
    |-------------------------------+----------------------+----------------------+
    | GPU  Name        Persistence-M| Bus-Id        Disp.A | Volatile Uncorr. ECC |
    | Fan  Temp  Perf  Pwr:Usage/Cap|         Memory-Usage | GPU-Util  Compute M. |
    |===============================+======================+======================|
    |   0  Tesla M60           Off  | 0000:04:00.0     Off |                    0 |
    | N/A   34C    P0    39W / 150W |     34MiB /  7679MiB |      0%      Default |
    +-------------------------------+----------------------+----------------------+
    |   1  Tesla M60           Off  | 0000:05:00.0     Off |                    0 |
    | N/A   35C    P0    39W / 150W |     34MiB /  7679MiB |      0%      Default |
    +-------------------------------+----------------------+----------------------+
    |   2  Tesla M60           Off  | 0000:83:00.0     Off |                    0 |
    | N/A   38C    P0    38W / 150W |     34MiB /  7679MiB |      0%      Default |
    +-------------------------------+----------------------+----------------------+
    |   3  Tesla M60           Off  | 0000:84:00.0     Off |                    0 |
    | N/A   34C    P0    38W / 150W |     34MiB /  7679MiB |     99%      Default |
    +-------------------------------+----------------------+----------------------+

    +-----------------------------------------------------------------------------+
    | Processes:                                                       GPU Memory |
    |  GPU       PID  Type  Process name                               Usage      |
    |=============================================================================|
    |  No running processes found                                                 |
    +-----------------------------------------------------------------------------+

To verify your CUDA installation, it is recommended to go through the instructions at the link below:

    http://docs.nvidia.com/cuda/cuda-getting-started-guide-for-linux/#install-samples

Finally, you should get a developer to run Mesos's Nvidia GPU-related
unit tests on your machine to ensure that everything passes (as
described below).

### Running Mesos Unit Tests

At the time of this writing, the following Nvidia GPU specific unit
tests exist on Mesos:

    DockerTest.ROOT_DOCKER_NVIDIA_GPU_DeviceAllow
    DockerTest.ROOT_DOCKER_NVIDIA_GPU_InspectDevices
    NvidiaGpuTest.ROOT_CGROUPS_NVIDIA_GPU_VerifyDeviceAccess
    NvidiaGpuTest.ROOT_INTERNET_CURL_CGROUPS_NVIDIA_GPU_NvidiaDockerImage
    NvidiaGpuTest.ROOT_CGROUPS_NVIDIA_GPU_FractionalResources
    NvidiaGpuTest.NVIDIA_GPU_Discovery
    NvidiaGpuTest.ROOT_CGROUPS_NVIDIA_GPU_FlagValidation
    NvidiaGpuTest.NVIDIA_GPU_Allocator
    NvidiaGpuTest.ROOT_NVIDIA_GPU_VolumeCreation
    NvidiaGpuTest.ROOT_NVIDIA_GPU_VolumeShouldInject)

The capitalized words following the `'.'` specify test filters to
apply when running the unit tests. In our case the filters that apply
are `ROOT`, `CGROUPS`, and `NVIDIA_GPU`. This means that these tests
must be run as `root` on Linux machines with `cgroups` support that
have Nvidia GPUs installed on them. The check to verify that Nvidia
GPUs exist is to look for the existence of the Nvidia System
Management Interface (`nvidia-smi`) on the machine where the tests are
being run. This binary should already be installed if the instructions
above have been followed correctly.

So long as these filters are satisfied, you can run the following to
execute these unit tests:

    [mesos]$ GTEST_FILTER="" make -j check
    [mesos]$ sudo bin/mesos-tests.sh --gtest_filter="*NVIDIA_GPU*"
