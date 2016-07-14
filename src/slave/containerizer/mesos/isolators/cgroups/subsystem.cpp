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

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "slave/containerizer/mesos/isolators/cgroups/subsystem.hpp"

using mesos::slave::ContainerLimitation;

using process::Failure;
using process::Future;
using process::Owned;
using process::PID;

using std::string;

namespace mesos {
namespace internal {
namespace slave {

Try<Owned<Subsystem>> Subsystem::create(
    const Flags& _flags,
    const string& _name,
    const string& _hierarchy)
{
  return Error("Not implemented.");
}


Subsystem::Subsystem(
    const Flags& _flags,
    const string& _hierarchy)
  : flags(_flags),
    hierarchy(_hierarchy) {}


Subsystem::~Subsystem()
{
}


Future<Nothing> Subsystem::recover(const ContainerID& containerId)
{
  return Failure("Not implemented.");
}


Future<Nothing> Subsystem::prepare(const ContainerID& containerId)
{
  return Failure("Not implemented.");
}


Future<Nothing> Subsystem::isolate(const ContainerID& containerId, pid_t pid)
{
  return Failure("Not implemented.");
}


Future<Nothing> Subsystem::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return Failure("Not implemented.");
}


Future<ResourceStatistics> Subsystem::usage(const ContainerID& containerId)
{
  return Failure("Not implemented.");
}


Future<ContainerStatus> Subsystem::status(const ContainerID& containerId)
{
  return Failure("Not implemented.");
}


Future<Nothing> Subsystem::cleanup(const ContainerID& containerId)
{
  return Failure("Not implemented.");
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
