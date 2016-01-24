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
#include <vector>

#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/pid.hpp>

#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/net_cls.hpp"

using std::list;
using std::set;
using std::string;
using std::vector;

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

CgroupsNetClsIsolatorProcess::CgroupsNetClsIsolatorProcess(
    const Flags& _flags,
    const string& _hierarchy)
  : flags(_flags),
    hierarchy(_hierarchy) {}


CgroupsNetClsIsolatorProcess::~CgroupsNetClsIsolatorProcess() {}


Try<Isolator*> CgroupsNetClsIsolatorProcess::create(const Flags& flags)
{
  Try<string> hierarchy = cgroups::prepare(
      flags.cgroups_hierarchy,
      "net_cls",
      flags.cgroups_root);

  if (hierarchy.isError()) {
    return Error("Failed to create net_cls cgroup: " + hierarchy.error());
  }

  // Ensure that no other subsystem is attached to the hierarchy.
  Try<set<string>> subsystems = cgroups::subsystems(hierarchy.get());
  if (subsystems.isError()) {
    return Error(
        "Failed to get the list of attached subsystems for hierarchy " +
        hierarchy.get());
  } else if (subsystems.get().size() != 1) {
    return Error(
        "Unexpected subsystems found attached to the hierarchy " +
        hierarchy.get());
  }

  process::Owned<MesosIsolatorProcess> process(
      new CgroupsNetClsIsolatorProcess(flags, hierarchy.get()));

  return new MesosIsolator(process);
}


Future<Nothing> CgroupsNetClsIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();
    const string cgroup = path::join(flags.cgroups_root, containerId.value());

    Try<bool> exists = cgroups::exists(hierarchy, cgroup);
    if (exists.isError()) {
      infos.clear();
      return Failure("Failed to check cgroup for container '" +
                     stringify(containerId) + "'");
    }

    if (!exists.get()) {
      VLOG(1) << "Couldn't find cgroup for container " << containerId;
      // This may occur if the executor has exited and the isolator
      // has destroyed the cgroup but the slave dies before noticing
      // this. This will be detected when the containerizer tries to
      // monitor the executor's pid.
      continue;
    }

    infos.emplace(containerId, Info(cgroup));
  }

  // Remove orphan cgroups.
  Try<vector<string>> cgroups = cgroups::get(hierarchy, flags.cgroups_root);
  if (cgroups.isError()) {
    infos.clear();
    return Failure(cgroups.error());
  }

  foreach (const string& cgroup, cgroups.get()) {
    // Ignore the slave cgroup (see the --slave_subsystems flag).
    // TODO(idownes): Remove this when the cgroups layout is updated,
    // see MESOS-1185.
    if (cgroup == path::join(flags.cgroups_root, "slave")) {
      continue;
    }

    ContainerID containerId;
    containerId.set_value(Path(cgroup).basename());

    if (infos.contains(containerId)) {
      continue;
    }

    // Known orphan cgroups will be destroyed by the containerizer
    // using the normal cleanup path. See MESOS-2367 for details.
    if (orphans.contains(containerId)) {
      infos.emplace(containerId, Info(cgroup));
      continue;
    }

    LOG(INFO) << "Removing unknown orphaned cgroup '" << cgroup << "'";

    // We don't wait on the destroy as we don't want to block recovery.
    cgroups::destroy(hierarchy, cgroup, cgroups::DESTROY_TIMEOUT);
  }

  return Nothing();
}


// TODO(asridharan): Currently we haven't decided on the entity who will
// allocate the net_cls handles, or the interfaces through which the net_cls
// handles will be exposed to network isolators and frameworks. Once the
// management entity is decided we might need to revisit this implementation.
Future<Nothing> CgroupsNetClsIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return Nothing();
}


// The net_cls handles aren't treated as resources. Further, they have fixed
// values and hence don't have a notion of usage. We are therefore returning an
// empty 'ResourceStatistics' object.
Future<ResourceStatistics> CgroupsNetClsIsolatorProcess::usage(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  return ResourceStatistics();
}


Future<Option<ContainerLaunchInfo>> CgroupsNetClsIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  // Use this info to create the cgroup, but do not insert it into
  // infos till the cgroup has been created successfully.
  Info info(path::join(flags.cgroups_root, containerId.value()));

  // Create a cgroup for this container.
  Try<bool> exists = cgroups::exists(hierarchy, info.cgroup);
  if (exists.isError()) {
    return Failure("Failed to check if the cgroup already exists: " +
                   exists.error());
  } else if (exists.get()) {
    return Failure("The cgroup already exists");
  }

  Try<Nothing> create = cgroups::create(hierarchy, info.cgroup);
  if (create.isError()) {
    return Failure("Failed to create the cgroup: " + create.error());
  }

  // 'chown' the cgroup so the executor can create nested cgroups. Do
  // not recurse so the control files are still owned by the slave
  // user and thus cannot be changed by the executor.
  if (containerConfig.has_user()) {
    Try<Nothing> chown = os::chown(
        containerConfig.user(),
        path::join(hierarchy, info.cgroup),
        false);

    if (chown.isError()) {
      return Failure("Failed to change ownership of cgroup hierarchy: " +
                     chown.error());
    }
  }

  infos.emplace(containerId, info);

  return update(containerId, containerConfig.executorinfo().resources())
    .then([]() -> Future<Option<ContainerLaunchInfo>> {
      return None();
    });
}


Future<Nothing> CgroupsNetClsIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  const Info& info = infos.at(containerId);

  Try<Nothing> assign = cgroups::assign(hierarchy, info.cgroup, pid);
  if (assign.isError()) {
    return Failure("Failed to assign container '" +
                   stringify(containerId) + "' to its own cgroup '" +
                   path::join(hierarchy, info.cgroup) +
                   "': " + assign.error());
  }

  return Nothing();
}


// The net_cls handles are labels and hence there are no limitations associated
// with them . This function would therefore always return a pending future
// since the limitation is never reached.
Future<ContainerLimitation> CgroupsNetClsIsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  return Future<ContainerLimitation>();
}


Future<Nothing> CgroupsNetClsIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // Multiple calls may occur during test clean up.
  if (!infos.contains(containerId)) {
    VLOG(1) << "Ignoring cleanup request for unknown container: "
            << containerId;
    return Nothing();
  }

  const Info& info = infos.at(containerId);

  return cgroups::destroy(hierarchy, info.cgroup, cgroups::DESTROY_TIMEOUT)
    .then(defer(PID<CgroupsNetClsIsolatorProcess>(this), [=]() {
      infos.erase(containerId);
      return Nothing();
    }));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
