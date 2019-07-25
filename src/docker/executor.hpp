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

#ifndef __DOCKER_EXECUTOR_HPP__
#define __DOCKER_EXECUTOR_HPP__

#include <stdio.h>

#include <map>
#include <string>

#include <mesos/executor.hpp>

#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/option.hpp>

#include "docker/docker.hpp"

#include "logging/flags.hpp"

namespace mesos {
namespace internal {
namespace docker {

struct Flags : public virtual mesos::internal::logging::Flags
{
  Flags() {
    add(&Flags::container,
        "container",
        "The name of the docker container to run.");

    add(&Flags::docker,
        "docker",
        "The path to the docker executable.");

    add(&Flags::docker_socket,
        "docker_socket",
        "Resource used by the agent and the executor to provide CLI access\n"
        "to the Docker daemon. On Unix, this is typically a path to a\n"
        "socket, such as '/var/run/docker.sock'. On Windows this must be a\n"
        "named pipe, such as '//./pipe/docker_engine'.");

    add(&Flags::sandbox_directory,
        "sandbox_directory",
        "The path to the container sandbox holding stdout and stderr files\n"
        "into which docker container logs will be redirected.");

    add(&Flags::mapped_directory,
        "mapped_directory",
        "The sandbox directory path that is mapped in the docker container.");

    // TODO(alexr): Remove this after the deprecation cycle (started in 1.0).
    add(&Flags::stop_timeout,
        "stop_timeout",
        "The duration for docker to wait after stopping a running container\n"
        "before it kills that container. This flag is deprecated; use task's\n"
        "kill policy instead.");

    add(&Flags::launcher_dir,
        "launcher_dir",
        "Directory path of Mesos binaries. Mesos would find fetcher,\n"
        "containerizer and executor binary files under this directory.");

    add(&Flags::task_environment,
        "task_environment",
        "A JSON map of environment variables and values that should\n"
        "be passed into the task launched by this executor.");

    add(&Flags::default_container_dns,
        "default_container_dns",
        "JSON-formatted default DNS information for container.");

    add(&Flags::cgroups_enable_cfs,
        "cgroups_enable_cfs",
        "Cgroups feature flag to enable hard limits on CPU resources\n"
        "via the CFS bandwidth limiting subfeature.\n",
        false);
  }

  Option<std::string> container;
  Option<std::string> docker;
  Option<std::string> docker_socket;
  Option<std::string> sandbox_directory;
  Option<std::string> mapped_directory;
  Option<std::string> launcher_dir;
  Option<std::string> task_environment;
  Option<std::string> default_container_dns;

  bool cgroups_enable_cfs;

  // TODO(alexr): Remove this after the deprecation cycle (started in 1.0).
  Option<Duration> stop_timeout;
};


class DockerExecutorProcess;


class DockerExecutor : public Executor
{
public:
  DockerExecutor(
      const process::Owned<Docker>& docker,
      const std::string& container,
      const std::string& sandboxDirectory,
      const std::string& mappedDirectory,
      const Duration& shutdownGracePeriod,
      const std::string& launcherDir,
      const std::map<std::string, std::string>& taskEnvironment,
      const Option<ContainerDNSInfo>& defaultContainerDNS,
      bool cgroupsEnableCfs);

  ~DockerExecutor() override;

  void registered(
      ExecutorDriver* driver,
      const ExecutorInfo& executorInfo,
      const FrameworkInfo& frameworkInfo,
      const SlaveInfo& slaveInfo) override;

  void reregistered(
      ExecutorDriver* driver,
      const SlaveInfo& slaveInfo) override;

  void disconnected(ExecutorDriver* driver) override;

  void launchTask(ExecutorDriver* driver, const TaskInfo& task) override;

  void killTask(ExecutorDriver* driver, const TaskID& taskId) override;

  void frameworkMessage(
      ExecutorDriver* driver,
      const std::string& data) override;

  void shutdown(ExecutorDriver* driver) override;

  void error(ExecutorDriver* driver, const std::string& data) override;

  void killTask(
      ExecutorDriver* driver,
      const TaskID& taskId,
      const Option<KillPolicy>& killPolicyOverride);

private:
  process::Owned<DockerExecutorProcess> process;
};

} // namespace docker {
} // namespace internal {
} // namespace mesos {

#endif // __DOCKER_EXECUTOR_HPP__
