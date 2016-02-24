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

#include <list>
#include <string>

#include <glog/logging.h>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <mesos/docker/v1.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/docker/runtime.hpp"

using std::list;
using std::string;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

DockerRuntimeIsolatorProcess::DockerRuntimeIsolatorProcess(
    const Flags& _flags)
  : flags(_flags) {}


DockerRuntimeIsolatorProcess::~DockerRuntimeIsolatorProcess() {}


Try<Isolator*> DockerRuntimeIsolatorProcess::create(const Flags& flags)
{
  process::Owned<MesosIsolatorProcess> process(
      new DockerRuntimeIsolatorProcess(flags));

  return new MesosIsolator(process);
}


Future<Nothing> DockerRuntimeIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  return Nothing();
}


Future<Option<ContainerLaunchInfo>> DockerRuntimeIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  const ExecutorInfo& executorInfo = containerConfig.executor_info();

  if (!executorInfo.has_container()) {
    return None();
  }

  if (executorInfo.container().type() != ContainerInfo::MESOS) {
    return Failure("Can only prepare docker runtime for a MESOS container");
  }

  if (!containerConfig.has_docker()) {
    // No docker image default config available.
    return None();
  }

  Option<Environment> environment =
    getLaunchEnvironment(containerId, containerConfig);

  Option<string> workingDirectory =
    getWorkingDirectory(containerConfig);

  Result<CommandInfo> command =
    getExecutorLaunchCommand(containerId, containerConfig);

  if (command.isError()) {
    return Failure("Failed to determine the executor launch command: " +
                   command.error());
  }

  // Set 'launchInfo'.
  ContainerLaunchInfo launchInfo;

  if (environment.isSome()) {
    launchInfo.mutable_environment()->CopyFrom(environment.get());
  }

  // If working directory or command exists, operation has to be
  // distinguished for either custom executor or command task. For
  // custom executor case, info will be included in 'launchInfo', and
  // will be passed back to containerizer. For command task case, info
  // will be passed to command executor as flags.
  if (!containerConfig.has_task_info()) {
    // Custom executor case.
    if (workingDirectory.isSome()) {
      launchInfo.set_working_directory(workingDirectory.get());
    }

    if (command.isSome()) {
      launchInfo.mutable_command()->CopyFrom(command.get());
    }
  } else {
    // Command task case. The 'executorCommand' below is the
    // command with value as 'mesos-executor'.
    CommandInfo executorCommand = containerConfig.executor_info().command();

    // Pass working directory to command executor as a flag.
    if (workingDirectory.isSome()) {
      executorCommand.add_arguments(
          "--working_directory=" + workingDirectory.get());
    }

    // Pass task command as a flag, which will be loaded by
    // command executor.
    if (command.isSome()) {
      executorCommand.add_arguments(
          "--task_command=" +
          stringify(JSON::protobuf(command.get())));
    }

    launchInfo.mutable_command()->CopyFrom(executorCommand);
  }

  return launchInfo;
}


Option<Environment> DockerRuntimeIsolatorProcess::getLaunchEnvironment(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  CHECK(containerConfig.docker().manifest().has_config());

  if (containerConfig.docker().manifest().config().env_size() == 0) {
    return None();
  }

  Environment environment;

  foreach(const string& env,
          containerConfig.docker().manifest().config().env()) {
    // Use `find_first_of` to prevent to case that there are
    // multiple '=' existing.
    size_t position = env.find_first_of('=');
    if (position == string::npos) {
      VLOG(1) << "Skipping invalid environment variable: '"
              << env << "' in docker manifest for container "
              << containerId;

      continue;
    }

    const string name = env.substr(0, position);
    const string value = env.substr(position + 1);

    // Keep all environment from runtime isolator. If there exists
    // environment variable duplicate cases, it will be overwritten
    // in mesos containerizer.
    Environment::Variable* variable = environment.add_variables();
    variable->set_name(name);
    variable->set_value(value);
  }

  return environment;
}


// This method reads the CommandInfo form ExecutorInfo and optional
// TaskInfo, and merge them with docker image default Entrypoint and
// Cmd. It returns a merged CommandInfo which will be used to launch
// the executor. If no need to modify the command, this method will
// return none.
Result<CommandInfo> DockerRuntimeIsolatorProcess::getExecutorLaunchCommand(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  CHECK(containerConfig.docker().manifest().has_config());

  // We may or may not mutate the CommandInfo for executor depending
  // on the logic table below. For custom executor case, we make
  // changes to the command directly. For command task case, if no
  // need to change the launch command for the user task, we do not do
  // anything and return the CommandInfo from ExecutorInfo. We only
  // add a flag `--task_command` to carry command as a JSON object if
  // it is neccessary to mutate.
  CommandInfo command;

  if (!containerConfig.has_task_info()) {
    // Custom executor case.
    CHECK(containerConfig.executor_info().has_command());
    command = containerConfig.executor_info().command();
  } else {
    // Command task case.
    CHECK(containerConfig.task_info().has_command());
    command = containerConfig.task_info().command();
  }

  // We merge the CommandInfo following the logic:
  // 1. If 'shell' is true, we will ignore Entrypoint and Cmd from
  //    the docker image (row 5-8).
  // 2. If 'shell' is false and 'value' is set, we will ignore the
  //    Entrypoint and Cmd from the docker image as well (row 3-4).
  // 3. If 'shell' is false and 'value' is not set, use the docker
  //    image specified runtime configuration.
  //    i. If 'Entrypoint' is set, it is treated as executable,
  //       then 'Cmd' get appended as arguments.
  //    ii.If 'Entrypoint' is not set, use the first 'Cmd' as
  //       executable and the others as arguments.
  // +---------+--------------+--------------+--------------+--------------+
  // |         | Entrypoint=0 | Entrypoint=0 | Entrypoint=1 | Entrypoint=1 |
  // |         |     Cmd=0    |     Cmd=1    |     Cmd=0    |     Cmd=1    |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=0  |     Error    |   ./Cmd[0]   | ./Entrypt[0] | ./Entrypt[0] |
  // | value=0 |              |   Cmd[1]..   | Entrypt[1].. | Entrypt[1].. |
  // |  argv=0 |              |              |              |     Cmd..    |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=0  |     Error    |   ./Cmd[0]   | ./Entrypt[0] | ./Entrypt[0] |
  // | value=0 |              |     argv     | Entrypt[1].. | Entrypt[1].. |
  // |  argv=1 |              |              |     argv     |     argv     |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=0  |    ./value   |    ./value   |    ./value   |    ./value   |
  // | value=1 |              |              |              |              |
  // |  argv=0 |              |              |              |              |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=0  |    ./value   |    ./value   |    ./value   |    ./value   |
  // | value=1 |     argv     |     argv     |     argv     |     argv     |
  // |  argv=1 |              |              |              |              |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=1  |     Error    |     Error    |     Error    |     Error    |
  // | value=0 |              |              |              |              |
  // |  argv=0 |              |              |              |              |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=1  |     Error    |     Error    |     Error    |     Error    |
  // | value=0 |              |              |              |              |
  // |  argv=1 |              |              |              |              |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=1  |  /bin/sh -c  |  /bin/sh -c  |  /bin/sh -c  |  /bin/sh -c  |
  // | value=1 |     value    |     value    |     value    |     value    |
  // |  argv=0 |              |              |              |              |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=1  |  /bin/sh -c  |  /bin/sh -c  |  /bin/sh -c  |  /bin/sh -c  |
  // | value=1 |     value    |     value    |     value    |     value    |
  // |  argv=1 |              |              |              |              |
  // +---------+--------------+--------------+--------------+--------------+
  if (command.shell()) {
    if (!command.has_value()) {
      return Error("Shell specified but no command value provided");
    }

    // No need to mutate command (row 7-8).
    return None();
  }

  if (command.has_value()) {
    // No need to mutate command (row 3-4).
    return None();
  }

  // TODO(gilbert): Deprecate 'override' flag option in command task.
  // We do not exclude override case here.

  // We keep the arguments of commandInfo while it does not have a
  // value, so that arguments can be specified by user, which is
  // running with default image Entrypoint.
  const docker::spec::v1::ImageManifest::Config& config =
    containerConfig.docker().manifest().config();

  // Filter out executable for commandInfo value.
  if (config.entrypoint_size() > 0) {
    command.set_value(config.entrypoint(0));

    // Put user defined argv after default entrypoint argv
    // in sequence.
    command.clear_arguments();
    command.add_arguments(config.entrypoint(0));

    for (int i = 1; i < config.entrypoint_size(); i++) {
      command.add_arguments(config.entrypoint(i));
    }

    // Append all possible user argv after entrypoint arguments.
    if (!containerConfig.has_task_info()) {
      // Custom executor case.
      command.mutable_arguments()->MergeFrom(
          containerConfig.executor_info().command().arguments());
    } else {
      // Command task case.
      command.mutable_arguments()->MergeFrom(
          containerConfig.task_info().command().arguments());
    }

    // Overwrite default cmd arguments if CommandInfo arguments are
    // set by user. The logic below is the case that no argument is
    // set by user.
    if (command.arguments_size() == config.entrypoint_size()) {
      foreach (const string& cmd, config.cmd()) {
        command.add_arguments(cmd);
      }
    }
  } else if (config.cmd_size() > 0) {
    command.set_value(config.cmd(0));
    command.add_arguments(config.cmd(0));

    // Overwrite default cmd arguments if CommandInfo arguments
    // are set by user.
    if (command.arguments_size() == 1) {
      for (int i = 1; i < config.cmd_size(); i++) {
        command.add_arguments(config.cmd(i));
      }
    }
  } else {
    return Error("No executable is found");
  }

  return command;
}


Option<string> DockerRuntimeIsolatorProcess::getWorkingDirectory(
    const ContainerConfig& containerConfig)
{
  CHECK(containerConfig.docker().manifest().has_config());

  // NOTE: In docker manifest, if an image working directory is none,
  // it will be set as `"WorkingDir": ""`.
  if (!containerConfig.docker().manifest().config().has_workingdir() ||
      containerConfig.docker().manifest().config().workingdir() == "") {
    return None();
  }

  return containerConfig.docker().manifest().config().workingdir();
}


Future<Nothing> DockerRuntimeIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  return Nothing();
}


Future<ContainerLimitation> DockerRuntimeIsolatorProcess::watch(
    const ContainerID& containerId)
{
  return Future<ContainerLimitation>();
}


Future<Nothing> DockerRuntimeIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return Nothing();
}


Future<ResourceStatistics> DockerRuntimeIsolatorProcess::usage(
    const ContainerID& containerId)
{
  return ResourceStatistics();
}


Future<Nothing> DockerRuntimeIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
