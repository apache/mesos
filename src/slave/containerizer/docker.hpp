/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __DOCKER_CONTAINERIZER_HPP__
#define __DOCKER_CONTAINERIZER_HPP__

#include <process/shared.hpp>

#include <stout/hashset.hpp>

#include "docker/docker.hpp"

#include "slave/containerizer/containerizer.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Prefix used to name Docker containers in order to distinguish those
// created by Mesos from those created manually.
extern const std::string DOCKER_NAME_PREFIX;

// Directory that stores all the symlinked sandboxes that is mapped
// into Docker containers. This is a relative directory that will
// joined with the slave path. Only sandbox paths that contains a
// colon will be symlinked due to the limiitation of the Docker CLI.
extern const std::string DOCKER_SYMLINK_DIRECTORY;


// Forward declaration.
class DockerContainerizerProcess;


class DockerContainerizer : public Containerizer
{
public:
  static Try<DockerContainerizer*> create(
      const Flags& flags,
      Fetcher* fetcher);

  // This is only public for tests.
  DockerContainerizer(
      const Flags& flags,
      Fetcher* fetcher,
      process::Shared<Docker> docker);

  // This is only public for tests.
  DockerContainerizer(
      const process::Owned<DockerContainerizerProcess>& _process);

  virtual ~DockerContainerizer();

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  virtual void destroy(const ContainerID& containerId);

  virtual process::Future<hashset<ContainerID>> containers();

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
      process::Shared<Docker> _docker)
    : flags(_flags),
      fetcher(_fetcher),
      docker(_docker) {}

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  virtual void destroy(
      const ContainerID& containerId,
      bool killed = true); // process is either killed or reaped.

  virtual process::Future<Nothing> fetch(const ContainerID& containerId);

  virtual process::Future<Nothing> pull(
      const ContainerID& containerId,
      const std::string& directory,
      const std::string& image,
      bool forcePullImage);

  virtual process::Future<hashset<ContainerID>> containers();

private:
  // Continuations and helpers.
  process::Future<Nothing> _fetch(
      const ContainerID& containerId,
      const Option<int>& status);

  process::Future<Nothing> _pull(const std::string& image);

  Try<Nothing> checkpoint(
      const ContainerID& containerId,
      pid_t pid);

  process::Future<Nothing> _recover(
      const std::list<Docker::Container>& containers);

  process::Future<Nothing> _launch(
      const ContainerID& containerId);

  process::Future<Nothing> __launch(
      const ContainerID& containerId);

  // NOTE: This continuation is only applicable when launching a
  // container for a task.
  process::Future<pid_t> ___launch(
      const ContainerID& containerId);

  // NOTE: This continuation is only applicable when launching a
  // container for an executor.
  process::Future<Docker::Container> ____launch(
      const ContainerID& containerId);

  // NOTE: This continuation is only applicable when launching a
  // container for an executor.
  process::Future<pid_t> _____launch(
      const ContainerID& containerId,
      const Docker::Container& container);

  process::Future<bool> ______launch(
    const ContainerID& containerId,
    pid_t pid);

  void _destroy(
      const ContainerID& containerId,
      bool killed);

  void __destroy(
      const ContainerID& containerId,
      bool killed,
      const Future<Nothing>& future);

  void ___destroy(
      const ContainerID& containerId,
      bool killed,
      const Future<Option<int>>& status);

  process::Future<Nothing> _update(
      const ContainerID& containerId,
      const Resources& resources,
      const Docker::Container& container);

  process::Future<Nothing> __update(
      const ContainerID& containerId,
      const Resources& resources,
      pid_t pid);

  Future<ResourceStatistics> _usage(
      const ContainerID& containerId,
      const Docker::Container& container);

  Future<ResourceStatistics> __usage(
      const ContainerID& containerId,
      pid_t pid);

  // Call back for when the executor exits. This will trigger
  // container destroy.
  void reaped(const ContainerID& containerId);

  // Removes the docker container.
  void remove(const std::string& container);

  const Flags flags;

  Fetcher* fetcher;

  process::Shared<Docker> docker;

  struct Container
  {
    static Try<Container*> create(
        const ContainerID& id,
        const Option<TaskInfo>& taskInfo,
        const ExecutorInfo& executorInfo,
        const std::string& directory,
        const Option<std::string>& user,
        const SlaveID& slaveId,
        const process::PID<Slave>& slavePid,
        bool checkpoint,
        const Flags& flags);

    Container(const ContainerID& id)
      : state(FETCHING), id(id) {}

    Container(const ContainerID& id,
              const Option<TaskInfo>& taskInfo,
              const ExecutorInfo& executorInfo,
              const std::string& directory,
              const Option<std::string>& user,
              const SlaveID& slaveId,
              const process::PID<Slave>& slavePid,
              bool checkpoint,
              bool symlinked,
              const Flags& flags)
      : state(FETCHING),
        id(id),
        task(taskInfo),
        executor(executorInfo),
        directory(directory),
        user(user),
        slaveId(slaveId),
        slavePid(slavePid),
        checkpoint(checkpoint),
        symlinked(symlinked),
        flags(flags)
    {
      if (task.isSome()) {
        resources = task.get().resources();
      } else {
        resources = executor.resources();
      }
    }

    ~Container()
    {
      if (symlinked) {
        // The sandbox directory is a symlink, remove it at container
        // destroy.
        os::rm(directory);
      }
    }

    std::string name()
    {
      return DOCKER_NAME_PREFIX + stringify(id);
    }

    std::string image() const
    {
      if (task.isSome()) {
        return task.get().container().docker().image();
      }

      return executor.container().docker().image();
    }

    bool forcePullImage() const
    {
      if (task.isSome()) {
        return task.get().container().docker().force_pull_image();
      }

      return executor.container().docker().force_pull_image();
    }

    ContainerInfo container() const
    {
      if (task.isSome()) {
        return task.get().container();
      }

      return executor.container();
    }

    CommandInfo command() const
    {
      if (task.isSome()) {
        return task.get().command();
      }

      return executor.command();
    }

    // Returns any extra environment varaibles to set when launching
    // the Docker container (beyond the those found in CommandInfo).
    std::map<std::string, std::string> environment() const
    {
      if (task.isNone()) {
        return executorEnvironment(
            executor,
            directory,
            slaveId,
            slavePid,
            checkpoint,
            flags.recovery_timeout);
      }

      return std::map<std::string, std::string>();
    }

    // The DockerContainerier needs to be able to properly clean up
    // Docker containers, regardless of when they are destroyed. For
    // example, if a container gets destroyed while we are fetching,
    // we need to not keep running the fetch, nor should we try and
    // start the Docker container. For this reason, we've split out
    // the states into:
    //
    //     FETCHING
    //     PULLING
    //     RUNNING
    //     DESTROYING
    //
    // In particular, we made 'PULLING' be it's own state so that we
    // could easily destroy and cleanup when a user initiated pulling
    // a really big image but we timeout due to the executor
    // registration timeout. Since we curently have no way to discard
    // a Docker::run, we needed to explicitely do the pull (which is
    // the part that takes the longest) so that we can also explicitly
    // kill it when asked. Once the functions at Docker::* get support
    // for discarding, then we won't need to make pull be it's own
    // state anymore, although it doesn't hurt since it gives us
    // better error messages.
    enum State {
      FETCHING = 1,
      PULLING = 2,
      RUNNING = 3,
      DESTROYING = 4
    } state;

    ContainerID id;
    Option<TaskInfo> task;
    ExecutorInfo executor;

    // The sandbox directory for the container. This holds the
    // symlinked path if symlinked boolean is true.
    std::string directory;

    Option<std::string> user;
    SlaveID slaveId;
    process::PID<Slave> slavePid;
    bool checkpoint;
    bool symlinked;
    Flags flags;

    // Promise for future returned from wait().
    Promise<containerizer::Termination> termination;

    // Exit status of executor or container (depending on whether or
    // not we used the command executor). Represented as a promise so
    // that destroying can chain with it being set.
    Promise<Future<Option<int>>> status;

    // Future that tells us whether or not the run is still pending or
    // has failed so we know whether or not to wait for 'status'.
    Future<Nothing> run;

    // We keep track of the resources for each container so we can set
    // the ResourceStatistics limits in usage(). Note that this is
    // different than just what we might get from TaskInfo::resources
    // or ExecutorInfo::resources because they can change dynamically.
    Resources resources;

    // The docker pull future is stored so we can discard when
    // destroy is called while docker is pulling the image.
    Future<Docker::Image> pull;

    // Once the container is running, this saves the pid of the
    // running container.
    Option<pid_t> pid;
  };

  hashmap<ContainerID, Container*> containers_;
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __DOCKER_CONTAINERIZER_HPP__
