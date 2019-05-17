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

#include <process/id.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <mesos/docker/v1.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/docker/runtime.hpp"

using std::list;
using std::string;

using google::protobuf::RepeatedPtrField;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

DockerRuntimeIsolatorProcess::DockerRuntimeIsolatorProcess(
    const Flags& _flags)
  : ProcessBase(process::ID::generate("docker-runtime-isolator")),
    flags(_flags) {}


DockerRuntimeIsolatorProcess::~DockerRuntimeIsolatorProcess() {}


bool DockerRuntimeIsolatorProcess::supportsNesting()
{
  return true;
}


bool DockerRuntimeIsolatorProcess::supportsStandalone()
{
  return true;
}


Try<Isolator*> DockerRuntimeIsolatorProcess::create(const Flags& flags)
{
  process::Owned<MesosIsolatorProcess> process(
      new DockerRuntimeIsolatorProcess(flags));

  return new MesosIsolator(process);
}


Future<Option<ContainerLaunchInfo>> DockerRuntimeIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (flags.docker_ignore_runtime) {
    return None();
  }

  if (!containerConfig.has_container_info()) {
    return None();
  }

  if (containerConfig.container_info().type() != ContainerInfo::MESOS) {
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

  Option<string> user = getContainerUser(containerConfig);
  if (user.isSome()) {
    // TODO(gilbert): Parse the container user from 'user|uid[:group|gid]'
    // to corresponding user and group. UID and GID should be numerical,
    // while username and groupname should be non-numerical.
    // Please see:
    // https://github.com/docker/docker/blob/master/image/spec/v1.md#container-runconfig-field-descriptions // NOLINT

    // TODO(gilbert): Support container user once container capabilities
    // land. Currently, we just log a warning instead of a failure, so
    // images with user defined can still be executable by ROOT.
    LOG(WARNING) << "Container user '" << user.get() << "' is not "
                 << "supported yet for container " << containerId;
  }

  Result<CommandInfo> command =
    getLaunchCommand(containerId, containerConfig);

  if (command.isError()) {
    return Failure("Failed to determine the launch command: " +
                   command.error());
  }

  // Set 'launchInfo'.
  ContainerLaunchInfo launchInfo;

  // If working directory or command exists, operation has to be
  // handled specially for the command task. For the command task,
  // the working directory and task command will be passed to
  // command executor as flags. For custom executor, default
  // executor and nested container cases, these information will
  // be included in 'ContainerLaunchInfo', and will be passed
  // back to containerizer.
  if (containerConfig.has_task_info()) {
    // Command task case.
    if (environment.isSome()) {
      launchInfo.mutable_task_environment()->CopyFrom(environment.get());
    }

    // Pass working directory to command executor as a flag.
    if (workingDirectory.isSome()) {
      launchInfo.mutable_command()->add_arguments(
          "--working_directory=" + workingDirectory.get());
    }

    // Pass task command as a flag, which will be loaded by
    // command executor.
    if (command.isSome()) {
      launchInfo.mutable_command()->add_arguments(
          "--task_command=" +
          stringify(JSON::protobuf(command.get())));
    }
  } else {
    // The custom executor, default executor and nested container cases.
    if (environment.isSome()) {
      launchInfo.mutable_environment()->CopyFrom(environment.get());
    }

    if (workingDirectory.isSome()) {
      launchInfo.set_working_directory(workingDirectory.get());
    }

    if (command.isSome()) {
      launchInfo.mutable_command()->CopyFrom(command.get());
    }
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

  foreach (const string& env,
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


// This method reads the CommandInfo from ContainerConfig and optional
// TaskInfo, and merge them with docker image default Entrypoint and
// Cmd. It returns a merged CommandInfo which will be used to launch
// the docker container. If no need to modify the command, this method
// will return none.
Result<CommandInfo> DockerRuntimeIsolatorProcess::getLaunchCommand(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  CHECK(containerConfig.docker().manifest().has_config());

  // We may or may not mutate the CommandInfo for executor depending
  // on the logic table below. For custom executor, default executor
  // and nested container cases, we make changes to the command
  // directly. For command task case, if no need to change the launch
  // command for the user task, we do not do anything and return the
  // CommandInfo from ContainerConfig. We only add a flag
  // `--task_command` to carry command as a JSON object if it is
  // necessary to mutate.
  CommandInfo command;

  if (containerConfig.has_task_info()) {
    // Command task case.
    CHECK(containerConfig.task_info().has_command());
    command = containerConfig.task_info().command();
  } else {
    // Custom executor, default executor or nested container case.
    command = containerConfig.command_info();
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
    const RepeatedPtrField<string> arguments = command.arguments();
    command.clear_arguments();
    command.add_arguments(config.entrypoint(0));

    for (int i = 1; i < config.entrypoint_size(); i++) {
      command.add_arguments(config.entrypoint(i));
    }

    // Append all possible user argv after entrypoint arguments.
    command.mutable_arguments()->MergeFrom(arguments);

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

    // Put user defined argv after default cmd[0].
    const RepeatedPtrField<string> arguments = command.arguments();
    command.clear_arguments();
    command.add_arguments(config.cmd(0));

    // Append all possible user argv after cmd[0].
    command.mutable_arguments()->MergeFrom(arguments);

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


Option<string> DockerRuntimeIsolatorProcess::getContainerUser(
    const ContainerConfig& containerConfig)
{
  // NOTE: In docker manifest, if its container user is none, it may be
  // set as `"User": ""`.
  if (!containerConfig.docker().manifest().config().has_user() ||
      containerConfig.docker().manifest().config().user() == "") {
    return None();
  }

  return containerConfig.docker().manifest().config().user();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
