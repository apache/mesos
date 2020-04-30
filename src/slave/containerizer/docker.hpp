// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __DOCKER_CONTAINERIZER_HPP__
#define __DOCKER_CONTAINERIZER_HPP__

#include <map>
#include <set>
#include <string>

#include <mesos/slave/container_logger.hpp>

#include <process/owned.hpp>
#include <process/shared.hpp>

#include <process/metrics/metrics.hpp>
#include <process/metrics/timer.hpp>

#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/hashset.hpp>

#include "docker/docker.hpp"
#include "docker/executor.hpp"

#include "slave/containerizer/containerizer.hpp"

#include "slave/containerizer/mesos/isolators/gpu/components.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Prefix used to name Docker containers in order to distinguish those
// created by Mesos from those created manually.
extern const std::string DOCKER_NAME_PREFIX;

// Separator used to compose docker container name, which consists
// of the name prefix, ContainerID, and possibly the SlaveID depending
// on the version of Mesos used to create the container.
extern const std::string DOCKER_NAME_SEPERATOR;

// Directory that stores all the symlinked sandboxes that is mapped
// into Docker containers. This is a relative directory that will
// joined with the slave path. Only sandbox paths that contains a
// colon will be symlinked due to the limitation of the Docker CLI.
extern const std::string DOCKER_SYMLINK_DIRECTORY;


// Forward declaration.
class DockerContainerizerProcess;


class DockerContainerizer : public Containerizer
{
public:
  static Try<DockerContainerizer*> create(
      const Flags& flags,
      Fetcher* fetcher,
      const Option<NvidiaComponents>& nvidia = None());

  // This is only public for tests.
  DockerContainerizer(
      const Flags& flags,
      Fetcher* fetcher,
      const process::Owned<mesos::slave::ContainerLogger>& logger,
      process::Shared<Docker> docker,
      const Option<NvidiaComponents>& nvidia = None());

  // This is only public for tests.
  DockerContainerizer(
      const process::Owned<DockerContainerizerProcess>& _process);

  ~DockerContainerizer() override;

  process::Future<Nothing> recover(
      const Option<state::SlaveState>& state) override;

  process::Future<Containerizer::LaunchResult> launch(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig,
      const std::map<std::string, std::string>& environment,
      const Option<std::string>& pidCheckpointPath) override;

  process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<
          std::string, Value::Scalar>& resourceLimits = {}) override;

  process::Future<ResourceStatistics> usage(
      const ContainerID& containerId) override;

  process::Future<ContainerStatus> status(
      const ContainerID& containerId) override;

  process::Future<Option<mesos::slave::ContainerTermination>> wait(
      const ContainerID& containerId) override;

  process::Future<Option<mesos::slave::ContainerTermination>> destroy(
      const ContainerID& containerId) override;

  process::Future<hashset<ContainerID>> containers() override;

  process::Future<Nothing> pruneImages(
      const std::vector<Image>& excludedImages) override;

private:
  process::Owned<DockerContainerizerProcess> process;
};



class DockerContainerizerProcess
  : public process::Process<DockerContainerizerProcess>
{
public:
  DockerContainerizerProcess(
      const Flags& _flags,
      Fetcher* _fetcher,
      const process::Owned<mesos::slave::ContainerLogger>& _logger,
      process::Shared<Docker> _docker,
      const Option<NvidiaComponents>& _nvidia)
    : flags(_flags),
      fetcher(_fetcher),
      logger(_logger),
      docker(_docker),
      nvidia(_nvidia) {}

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<Containerizer::LaunchResult> launch(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig,
      const std::map<std::string, std::string>& environment,
      const Option<std::string>& pidCheckpointPath);

  // force = true causes the containerizer to update the resources
  // for the container, even if they match what it has cached.
  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<std::string, Value::Scalar>& resourceLimits,
      bool force);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<ContainerStatus> status(
      const ContainerID& containerId);

  virtual process::Future<Option<mesos::slave::ContainerTermination>> wait(
      const ContainerID& containerId);

  virtual process::Future<Option<mesos::slave::ContainerTermination>> destroy(
      const ContainerID& containerId,
      bool killed = true); // process is either killed or reaped.

  virtual process::Future<Nothing> fetch(const ContainerID& containerId);

  virtual process::Future<Nothing> pull(const ContainerID& containerId);

  virtual process::Future<hashset<ContainerID>> containers();

private:
  struct Metrics
  {
    Metrics() : image_pull("containerizer/docker/image_pull", Hours(1))
    {
      process::metrics::add(image_pull);
    }

    ~Metrics()
    {
      process::metrics::remove(image_pull);
    }

    process::metrics::Timer<Milliseconds> image_pull;
  };

  // Continuations and helpers.
  process::Future<Nothing> _fetch(
      const ContainerID& containerId,
      const Option<int>& status);

  Try<Nothing> checkpoint(
      const ContainerID& containerId,
      pid_t pid);

  process::Future<Containerizer::LaunchResult> _launch(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

  process::Future<Nothing> _recover(
      const Option<state::SlaveState>& state,
      const std::vector<Docker::Container>& containers);

  process::Future<Nothing> __recover(
      const std::vector<Docker::Container>& containers);

  // Starts the executor in a Docker container.
  process::Future<Docker::Container> launchExecutorContainer(
      const ContainerID& containerId,
      const std::string& containerName);

  // Starts the docker executor with a subprocess.
  process::Future<pid_t> launchExecutorProcess(
      const ContainerID& containerId);

  process::Future<pid_t> checkpointExecutor(
      const ContainerID& containerId,
      const Docker::Container& dockerContainer);

  // Reaps on the executor pid.
  process::Future<Nothing> reapExecutor(
      const ContainerID& containerId,
      pid_t pid);

  void _destroy(
      const ContainerID& containerId,
      bool killed);

  void __destroy(
      const ContainerID& containerId,
      bool killed,
      const process::Future<Nothing>& future);

  void ___destroy(
      const ContainerID& containerId,
      bool killed,
      const process::Future<Option<int>>& status);

  void ____destroy(
      const ContainerID& containerId,
      bool killed,
      const process::Future<Option<int>>& status);

  process::Future<Nothing> destroyTimeout(
      const ContainerID& containerId,
      process::Future<Nothing> future);

#ifdef __linux__
  process::Future<Nothing> _update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<std::string, Value::Scalar>& resourceLimits,
      const Docker::Container& container);

  process::Future<Nothing> __update(
      const ContainerID& containerId,
      const Resources& resourceRequests,
      const google::protobuf::Map<std::string, Value::Scalar>& resourceLimits);
#endif // __linux__

  process::Future<Nothing> mountPersistentVolumes(
      const ContainerID& containerId);

  Try<Nothing> unmountPersistentVolumes(
      const ContainerID& containerId);

  Try<Nothing> updatePersistentVolumes(
    const ContainerID& containerId,
    const std::string& directory,
    const Resources& current,
    const Resources& updated);

#ifdef __linux__
  // Allocate GPU resources for a specified container.
  process::Future<Nothing> allocateNvidiaGpus(
      const ContainerID& containerId,
      const size_t count);

  process::Future<Nothing> _allocateNvidiaGpus(
      const ContainerID& containerId,
      const std::set<Gpu>& allocated);

  // Deallocate GPU resources for a specified container.
  process::Future<Nothing> deallocateNvidiaGpus(
      const ContainerID& containerId);

  process::Future<Nothing> _deallocateNvidiaGpus(
      const ContainerID& containerId,
      const std::set<Gpu>& deallocated);
#endif // __linux__

  Try<ResourceStatistics> cgroupsStatistics(pid_t pid) const;

  // Call back for when the executor exits. This will trigger
  // container destroy.
  void reaped(const ContainerID& containerId);

  // Removes the docker container.
  void remove(
      const std::string& containerName,
      const Option<std::string>& executor);

  const Flags flags;

  Fetcher* fetcher;

  process::Owned<mesos::slave::ContainerLogger> logger;

  process::Shared<Docker> docker;

  Option<NvidiaComponents> nvidia;

  Metrics metrics;

  struct Container
  {
    static Try<Container*> create(
        const ContainerID& id,
        const mesos::slave::ContainerConfig& containerConfig,
        const std::map<std::string, std::string>& environment,
        const Option<std::string>& pidCheckpointPath,
        const Flags& flags);

    static std::string name(const ContainerID& id)
    {
      return DOCKER_NAME_PREFIX + stringify(id);
    }

    Container(const ContainerID& id)
      : state(FETCHING), id(id) {}

    Container(
        const ContainerID& _id,
        const mesos::slave::ContainerConfig& _containerConfig,
        const std::map<std::string, std::string>& _environment,
        const Option<std::string>& _pidCheckpointPath,
        bool symlinked,
        const std::string& containerWorkDir,
        const Option<CommandInfo>& _command,
        const Option<ContainerInfo>& _container,
        bool launchesExecutorContainer)
      : state(FETCHING),
        id(_id),
        containerConfig(_containerConfig),
        pidCheckpointPath(_pidCheckpointPath),
        environment(_environment),
        symlinked(symlinked),
        containerWorkDir(containerWorkDir),
        containerName(name(id)),
        launchesExecutorContainer(launchesExecutorContainer),
        generatedForCommandTask(_containerConfig.has_task_info())
    {
      // NOTE: The task's resources are included in the executor's
      // resources in order to make sure when launching the executor
      // that it has non-zero resources in the event the executor was
      // not actually given any resources by the framework
      // originally. See Framework::launchExecutor in slave.cpp. We
      // check that this is indeed the case here to protect ourselves
      // from when/if this changes in the future (but it's not a
      // perfect check because an executor might always have a subset
      // of it's resources that match a task, nevertheless, it's
      // better than nothing).
      resourceRequests = containerConfig.resources();
      resourceLimits = containerConfig.limits();

      if (containerConfig.has_task_info()) {
        CHECK(
            resourceRequests.contains(containerConfig.task_info().resources()));
      }

      if (_command.isSome()) {
        command = _command.get();
      } else {
        command = containerConfig.command_info();
      }

      if (_container.isSome()) {
        container = _container.get();
      } else {
        // NOTE: The existence of this field is checked in
        // DockerContainerizerProcess::launch.
        container = containerConfig.container_info();
      }
    }

    ~Container()
    {
      if (symlinked) {
        // The sandbox directory is a symlink, remove it at container
        // destroy.
        os::rm(containerWorkDir);
      }
    }

    Option<std::string> executorName()
    {
      if (launchesExecutorContainer) {
        return containerName + DOCKER_NAME_SEPERATOR + "executor";
      } else {
        return None();
      }
    }

    std::string image() const
    {
      if (containerConfig.has_task_info()) {
        return containerConfig.task_info().container().docker().image();
      }

      return containerConfig.executor_info().container().docker().image();
    }

    bool forcePullImage() const
    {
      if (containerConfig.has_task_info()) {
        return containerConfig.task_info()
          .container().docker().force_pull_image();
      }

      return containerConfig.executor_info()
        .container().docker().force_pull_image();
    }

    // The DockerContainerizer needs to be able to properly clean up
    // Docker containers, regardless of when they are destroyed. For
    // example, if a container gets destroyed while we are fetching,
    // we need to not keep running the fetch, nor should we try and
    // start the Docker container. For this reason, we've split out
    // the states into:
    //
    //     FETCHING
    //     PULLING
    //     MOUNTING
    //     RUNNING
    //     DESTROYING
    //
    // In particular, we made 'PULLING' be it's own state so that we
    // can easily destroy and cleanup when a user initiated pulling
    // a really big image but we timeout due to the executor
    // registration timeout. Since we currently have no way to discard
    // a Docker::run, we needed to explicitly do the pull (which is
    // the part that takes the longest) so that we can also explicitly
    // kill it when asked. Once the functions at Docker::* get support
    // for discarding, then we won't need to make pull be it's own
    // state anymore, although it doesn't hurt since it gives us
    // better error messages.
    enum State
    {
      FETCHING = 1,
      PULLING = 2,
      MOUNTING = 3,
      RUNNING = 4,
      DESTROYING = 5
    } state;

    // Copies of the parameters sent to `Container::create`.
    const ContainerID id;
    const mesos::slave::ContainerConfig containerConfig;
    const Option<std::string> pidCheckpointPath;

    // A copy of the parameter sent to `Container::create`.
    // NOTE: This may be modified further by hooks.
    std::map<std::string, std::string> environment;

    // The sandbox directory for the container. This holds the
    // symlinked path if symlinked boolean is true.
    // TODO(josephw): The symlink path does not persist across failovers,
    // so we will not delete the symlink if the agent restarts. This results
    // in gradually leaking hanging symlinks.
    bool symlinked;
    std::string containerWorkDir;

    // Copies of the fields in `containerConfig`, except when the
    // container is a command task and the agent is launched with
    // the --docker_mesos_image flag.
    ContainerInfo container;
    CommandInfo command;

    // Environment variables that the command executor should pass
    // onto a docker-ized task. This is set by a hook.
    Option<std::map<std::string, std::string>> taskEnvironment;

    // The string used to refer to this container via the Docker CLI.
    // This name is either computed by concatenating the DOCKER_NAME_PREFIX
    // and the ContainerID; or during recovery, by taking the recovered
    // container's name.
    std::string containerName;

    // Promise for future returned from wait().
    process::Promise<mesos::slave::ContainerTermination> termination;

    // Exit status of executor or container (depending on whether or
    // not we used the command executor). Represented as a promise so
    // that destroying can chain with it being set.
    process::Promise<process::Future<Option<int>>> status;

    // Future that tells us the return value of last launch stage (fetch, pull,
    // run, etc).
    process::Future<Containerizer::LaunchResult> launch;

    // We keep track of the resources for each container so we can set
    // the ResourceStatistics limits in usage(). Note that this is
    // different than just what we might get from TaskInfo::resources
    // or ExecutorInfo::resources because they can change dynamically.
    Resources resourceRequests;
    google::protobuf::Map<std::string, Value::Scalar> resourceLimits;

    // The docker pull future is stored so we can discard when
    // destroy is called while docker is pulling the image.
    process::Future<Docker::Image> pull;

    // Once the container is running, this saves the pid of the
    // running container.
    Option<pid_t> pid;

    // The executor pid that was forked to wait on the running
    // container. This is stored so we can clean up the executor
    // on destroy.
    Option<pid_t> executorPid;

#ifdef __linux__
    // GPU resources allocated to the container.
    std::set<Gpu> gpus;

    Option<std::string> cpuCgroup;
    Option<std::string> memoryCgroup;
#endif // __linux__

    // Marks if this container launches an executor in a docker
    // container.
    bool launchesExecutorContainer;

    bool generatedForCommandTask;
  };

  hashmap<ContainerID, Container*> containers_;
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __DOCKER_CONTAINERIZER_HPP__
