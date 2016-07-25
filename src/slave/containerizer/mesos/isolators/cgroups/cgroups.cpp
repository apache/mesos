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
#include <stout/foreach.hpp>
#include <stout/strings.hpp>

#include "linux/cgroups.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/cgroups.hpp"

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

using process::Failure;
using process::Future;
using process::Owned;

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

CgroupsIsolatorProcess::CgroupsIsolatorProcess(
    const Flags& _flags,
    const hashmap<string, string>& _hierarchies,
    const multihashmap<string, Owned<Subsystem>>& _subsystems)
  : flags(_flags),
    hierarchies(_hierarchies),
    subsystems(_subsystems) {}


CgroupsIsolatorProcess::~CgroupsIsolatorProcess() {}


Try<Isolator*> CgroupsIsolatorProcess::create(const Flags& flags)
{
  // Subsystem name -> hierarchy path.
  hashmap<string, string> hierarchies;

  // Hierarchy path -> subsystem object.
  multihashmap<string, Owned<Subsystem>> subsystems;

  // Multimap: isolator name -> subsystem name.
  multihashmap<string, string> isolatorMap = {
  };

  foreach (string isolator, strings::tokenize(flags.isolation, ",")) {
    if (!strings::startsWith(isolator, "cgroups/")) {
      // Skip when the isolator is not related to cgroups.
      continue;
    }

    isolator = strings::remove(isolator, "cgroups/", strings::Mode::PREFIX);

    // A cgroups isolator name may map to multiple subsystems. We need to
    // convert the isolator name to its associated subsystems.
    foreach (const string& subsystemName, isolatorMap.get(isolator)) {
      if (hierarchies.contains(subsystemName)) {
        // Skip when the subsystem exists.
        continue;
      }

      // Prepare hierarchy if it does not exist.
      Try<string> hierarchy = cgroups::prepare(
          flags.cgroups_hierarchy,
          subsystemName,
          flags.cgroups_root);

      if (hierarchy.isError()) {
        return Error(
            "Failed to prepare hierarchy for the subsystem '" + subsystemName +
            "': " + hierarchy.error());
      }

      // Create and load the subsystem.
      Try<Owned<Subsystem>> subsystem =
        Subsystem::create(flags, subsystemName, hierarchy.get());

      if (subsystem.isError()) {
        return Error(
            "Failed to create subsystem '" + subsystemName + "': " +
            subsystem.error());
      }

      subsystems.put(hierarchy.get(), subsystem.get());
      hierarchies.put(subsystemName, hierarchy.get());
    }
  }

  Owned<MesosIsolatorProcess> process(
      new CgroupsIsolatorProcess(flags, hierarchies, subsystems));

  return new MesosIsolator(process);
}


Future<Nothing> CgroupsIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  return Failure("Not implemented.");
}


Future<Option<ContainerLaunchInfo>> CgroupsIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  return Failure("Not implemented.");
}


Future<Nothing> CgroupsIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  return Failure("Not implemented.");
}


Future<ContainerLimitation> CgroupsIsolatorProcess::watch(
    const ContainerID& containerId)
{
  return Failure("Not implemented.");
}


Future<Nothing> CgroupsIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return Failure("Not implemented.");
}


Future<ResourceStatistics> CgroupsIsolatorProcess::usage(
    const ContainerID& containerId)
{
  return Failure("Not implemented.");
}


Future<ContainerStatus> CgroupsIsolatorProcess::status(
    const ContainerID& containerId)
{
  return Failure("Not implemented.");
}


Future<Nothing> CgroupsIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  return Failure("Not implemented.");
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
