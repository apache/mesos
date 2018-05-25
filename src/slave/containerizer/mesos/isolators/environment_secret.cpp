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

#include "slave/containerizer/mesos/isolators/environment_secret.hpp"

#include <vector>

#include <mesos/secret/resolver.hpp>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>

#include <stout/foreach.hpp>

#include "common/validation.hpp"

using std::vector;

using process::collect;
using process::Failure;
using process::Future;
using process::Owned;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

Try<Isolator*> EnvironmentSecretIsolatorProcess::create(
    const Flags& flags,
    SecretResolver* secretResolver)
{
  Owned<MesosIsolatorProcess> process(new EnvironmentSecretIsolatorProcess(
      flags,
      secretResolver));;

  return new MesosIsolator(process);
}


EnvironmentSecretIsolatorProcess::EnvironmentSecretIsolatorProcess(
    const Flags& _flags,
    SecretResolver* _secretResolver)
  : ProcessBase(process::ID::generate("environment-secret-isolator")),
    flags(_flags),
    secretResolver(_secretResolver) {}


EnvironmentSecretIsolatorProcess::~EnvironmentSecretIsolatorProcess() {}


bool EnvironmentSecretIsolatorProcess::supportsNesting()
{
  return true;
}


bool EnvironmentSecretIsolatorProcess::supportsStandalone()
{
  return true;
}


Future<Option<ContainerLaunchInfo>> EnvironmentSecretIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  Option<Error> error = common::validation::validateEnvironment(
      containerConfig.command_info().environment());
  if (error.isSome()) {
    return Failure( "Invalid environment specified: " + error->message);
  }

  Environment environment;

  vector<Future<Environment::Variable>> futures;
  foreach (const Environment::Variable& variable,
           containerConfig.command_info().environment().variables()) {
    if (variable.type() != Environment::Variable::SECRET) {
      continue;
    }

    const Secret& secret = variable.secret();

    error = common::validation::validateSecret(secret);
    if (error.isSome()) {
      return Failure(
          "Invalid secret specified in environment '" + variable.name() +
          "': " + error->message);
    }

    if (secretResolver == nullptr) {
      return Failure(
          "Error: Environment variable '" + variable.name() +
          "' contains secret but no secret resolver provided");
    }

    Future<Environment::Variable> future = secretResolver->resolve(secret)
      .then([variable](const Secret::Value& secretValue)
            -> Future<Environment::Variable> {
          Environment::Variable result;
          result.set_name(variable.name());
          result.set_value(secretValue.data());
          return result;
        });

    futures.push_back(future);
  }

  return collect(futures)
    .then([](const vector<Environment::Variable>& variables)
        -> Future<Option<ContainerLaunchInfo>> {
      ContainerLaunchInfo launchInfo;
      Environment* environment = launchInfo.mutable_environment();
      foreach (const Environment::Variable& variable, variables) {
        environment->add_variables()->CopyFrom(variable);
      }
      launchInfo.mutable_task_environment()->CopyFrom(*environment);
      return launchInfo;
    });
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
