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

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/appc/runtime.hpp"

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

AppcRuntimeIsolatorProcess::AppcRuntimeIsolatorProcess(const Flags& _flags)
  : ProcessBase(process::ID::generate("appc-runtime-isolator")),
    flags(_flags) {}


AppcRuntimeIsolatorProcess::~AppcRuntimeIsolatorProcess() {}


bool AppcRuntimeIsolatorProcess::supportsNesting()
{
  return true;
}


bool AppcRuntimeIsolatorProcess::supportsStandalone()
{
  return true;
}


Try<Isolator*> AppcRuntimeIsolatorProcess::create(const Flags& flags)
{
  process::Owned<MesosIsolatorProcess> process(
      new AppcRuntimeIsolatorProcess(flags));

  return new MesosIsolator(process);
}


Future<Option<ContainerLaunchInfo>> AppcRuntimeIsolatorProcess::prepare(
    const ContainerID& containerId,
    const mesos::slave::ContainerConfig& containerConfig)
{
  if (!containerConfig.has_container_info()) {
    return None();
  }

  if (containerConfig.container_info().type() != ContainerInfo::MESOS) {
    return Failure("Can only prepare Appc runtime for a MESOS container");
  }

  if (!containerConfig.has_appc()) {
    // No Appc image default config available.
    return None();
  }

  Option<Environment> environment =
    getLaunchEnvironment(containerId, containerConfig);

  Option<string> workingDirectory = getWorkingDirectory(containerConfig);

  Result<CommandInfo> command = getLaunchCommand(containerId, containerConfig);

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
  // be included in 'ContainerLaunchInfo', and will be passed back
  // to containerizer.
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


Option<Environment> AppcRuntimeIsolatorProcess::getLaunchEnvironment(
    const ContainerID& containerId,
    const mesos::slave::ContainerConfig& containerConfig)
{
  if (!containerConfig.appc().manifest().has_app()) {
    return None();
  }

  if (containerConfig.appc().manifest().app().environment_size() == 0) {
    return None();
  }

  Environment environment;

  foreach (const appc::spec::ImageManifest::Environment& env,
           containerConfig.appc().manifest().app().environment()) {
    // Keep all environment from runtime isolator. If there exists
    // environment variable duplicate cases, it will be overwritten
    // in mesos containerizer.
    Environment::Variable* variable = environment.add_variables();
    variable->set_name(env.name());
    variable->set_value(env.value());
  }

  return environment;
}


// This method reads the CommandInfo from ContainerConfig and optional
// TaskInfo, and merge them with Appc image default 'exec'.
Result<CommandInfo> AppcRuntimeIsolatorProcess::getLaunchCommand(
    const ContainerID& containerId,
    const mesos::slave::ContainerConfig& containerConfig)
{
  if (!containerConfig.appc().manifest().has_app()) {
    return None();
  }

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
  // 1. If 'shell' is false, and 'value' is not set, use 'Exec' from the image.
  //    Rows 1 and 2 in the table below. On row 2 notice that arguments in the
  //    image are overwritten when arguments are provided by the user.
  // 2. If 'shell' is false and 'value' is set, ignore 'Exec' from the image.
  //    Rows 3 and 4 in the table below.
  // 3. If 'shell' is true and 'value' is not set, Error.
  //    Rows 5 and 6 in the table below.
  // 4. If 'shell' is true and 'value' is set, ignore 'Exec' from the image.
  //    Rows 7 and 8 in the table below.
  // +---------+---------------+---------------+
  // |         |     Exec=0    |     Exec=1    |
  // +---------+---------------+---------------+
  // |   sh=0  |     Error     |   ./Exec[0]   |
  // | value=0 |               |   Exec[1]..   |
  // |  argv=0 |               |               |
  // +---------+---------------+---------------+
  // |   sh=0  |     Error     |   ./Exec[0]   |
  // | value=0 |               |     argv      |
  // |  argv=1 |               |               |
  // +---------+---------------+---------------+
  // |   sh=0  |    ./value    |    ./value    |
  // | value=1 |               |               |
  // |  argv=0 |               |               |
  // +---------+---------------+---------------+
  // |   sh=0  |    ./value    |    ./value    |
  // | value=1 |     argv      |     argv      |
  // |  argv=1 |               |               |
  // +---------+---------------+---------------+
  // |   sh=1  |     Error     |     Error     |
  // | value=0 |               |               |
  // |  argv=0 |               |               |
  // +---------+---------------+---------------+
  // |   sh=1  |     Error     |     Error     |
  // | value=0 |               |               |
  // |  argv=1 |               |               |
  // +---------+---------------+---------------+
  // |   sh=1  |  /bin/sh -c   |  /bin/sh -c   |
  // | value=1 |     value     |     value     |
  // |  argv=0 |               |               |
  // +---------+---------------+---------------+
  // |   sh=1  |  /bin/sh -c   |  /bin/sh -c   |
  // | value=1 |     value     |     value     |
  // |  argv=1 |               |               |
  // +---------+---------------+---------------+
  if (command.shell()) {
    // Error that value is not set (row 5-6)
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

  const appc::spec::ImageManifest::App& app =
      containerConfig.appc().manifest().app();

  if (app.exec_size() > 0) {
    command.set_value(app.exec(0));

    const RepeatedPtrField<string> arguments = command.arguments();
    command.clear_arguments();
    command.add_arguments(app.exec(0));
    command.mutable_arguments()->MergeFrom(arguments);

    if (command.arguments_size() == 1) {
      for (int i = 1; i < app.exec_size(); i++) {
        command.add_arguments(app.exec(i));
      }
    }
  } else {
      return Error("No executable is found");
  }

  return command;
}


Option<string> AppcRuntimeIsolatorProcess::getWorkingDirectory(
    const mesos::slave::ContainerConfig& containerConfig)
{
  if (!containerConfig.appc().manifest().has_app()) {
    return None();
  }

  // NOTE: In Appc manifest, if an image working directory is none,
  // it will be set as `"workingDirectory": ""`.
  if (!containerConfig.appc().manifest().app().has_workingdirectory() ||
      containerConfig.appc().manifest().app().workingdirectory().empty()) {
    return None();
  }

  return containerConfig.appc().manifest().app().workingdirectory();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
