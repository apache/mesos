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

#include <stdint.h>

#include <vector>

#include <mesos/resources.hpp>
#include <mesos/values.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/pid.hpp>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/type_utils.hpp"

#include "linux/cgroups.hpp"

#include "slave/containerizer/isolators/cgroups/perf_event.hpp"

using namespace process;

using std::list;
using std::ostringstream;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

CgroupsPerfEventIsolatorProcess::CgroupsPerfEventIsolatorProcess(
    const Flags& _flags,
    const string& _hierarchy)
  : flags(_flags), hierarchy(_hierarchy) {}


CgroupsPerfEventIsolatorProcess::~CgroupsPerfEventIsolatorProcess() {}


Try<Isolator*> CgroupsPerfEventIsolatorProcess::create(const Flags& flags)
{
  Try<string> hierarchy = cgroups::prepare(
      flags.cgroups_hierarchy, "perf_event", flags.cgroups_root);

  if (hierarchy.isError()) {
    return Error("Failed to create perf_event cgroup: " + hierarchy.error());
  }

  process::Owned<IsolatorProcess> process(
      new CgroupsPerfEventIsolatorProcess(flags, hierarchy.get()));

  return new Isolator(process);
}


Future<Nothing> CgroupsPerfEventIsolatorProcess::recover(
    const list<state::RunState>& states)
{
  hashset<string> cgroups;

  foreach (const state::RunState& state, states) {
    if (state.id.isNone()) {
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      return Failure("ContainerID is required to recover");
    }

    const ContainerID& containerId = state.id.get();

    Info* info = new Info(
        containerId, path::join(flags.cgroups_root, containerId.value()));
    CHECK_NOTNULL(info);

    infos[containerId] = info;
    cgroups.insert(info->cgroup);

    Try<bool> exists = cgroups::exists(hierarchy, info->cgroup);
    if (exists.isError()) {
      delete info;
      foreachvalue (Info* info, infos) {
        delete info;
      }
      infos.clear();
      return Failure("Failed to check cgroup for container '" +
                     stringify(containerId) + "'");
    }

    if (!exists.get()) {
      VLOG(1) << "Couldn't find cgroup for container " << containerId;
      // This may occur if the executor is exiting and the isolator has
      // destroyed the cgroup but the slave dies before noticing this. This
      // will be detected when the containerizer tries to monitor the
      // executor's pid.
      // NOTE: This could also occur if this isolator is now enabled for a
      // container that was started without this isolator. For this particular
      // isolator it is okay to continue running this container without its
      // perf_event cgroup existing because we don't ever query it and the
      // destroy will succeed immediately.
    }
  }

  Try<vector<string> > orphans = cgroups::get(
      hierarchy, flags.cgroups_root);
  if (orphans.isError()) {
    foreachvalue (Info* info, infos) {
      delete info;
    }
    infos.clear();
    return Failure(orphans.error());
  }

  foreach (const string& orphan, orphans.get()) {
    if (!cgroups.contains(orphan)) {
      LOG(INFO) << "Removing orphaned cgroup '" << orphan << "'";
      cgroups::destroy(hierarchy, orphan);
    }
  }

  return Nothing();
}


Future<Option<CommandInfo> > CgroupsPerfEventIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  Info* info = new Info(
      containerId, path::join(flags.cgroups_root, containerId.value()));

  infos[containerId] = CHECK_NOTNULL(info);

  // Create a cgroup for this container.
  Try<bool> exists = cgroups::exists(hierarchy, info->cgroup);

  if (exists.isError()) {
    return Failure("Failed to prepare isolator: " + exists.error());
  }

  if (exists.get()) {
    return Failure("Failed to prepare isolator: cgroup already exists");
  }

  if (!exists.get()) {
    Try<Nothing> create = cgroups::create(hierarchy, info->cgroup);
    if (create.isError()) {
      return Failure("Failed to prepare isolator: " + create.error());
    }
  }

  return None();
}


Future<Nothing> CgroupsPerfEventIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  CHECK(info->pid.isNone());
  info->pid = pid;

  Try<Nothing> assign = cgroups::assign(hierarchy, info->cgroup, pid);
  if (assign.isError()) {
    return Failure("Failed to assign container '" +
                   stringify(info->containerId) + "' to its own cgroup '" +
                   path::join(hierarchy, info->cgroup) +
                   "' : " + assign.error());
  }

  return Nothing();
}


Future<Limitation> CgroupsPerfEventIsolatorProcess::watch(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  CHECK_NOTNULL(infos[containerId]);

  return infos[containerId]->limitation.future();
}


Future<Nothing> CgroupsPerfEventIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  // Nothing to update.
  return Nothing();
}


Future<ResourceStatistics> CgroupsPerfEventIsolatorProcess::usage(
    const ContainerID& containerId)
{
  // No resource statistics provided by this isolator.
  return ResourceStatistics();
}


Future<Nothing> CgroupsPerfEventIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  Info* info = CHECK_NOTNULL(infos[containerId]);

  return cgroups::destroy(hierarchy, info->cgroup)
    .then(defer(PID<CgroupsPerfEventIsolatorProcess>(this),
                &CgroupsPerfEventIsolatorProcess::_cleanup,
                containerId));
}


Future<Nothing> CgroupsPerfEventIsolatorProcess::_cleanup(
    const ContainerID& containerId)
{
  CHECK(infos.contains(containerId));

  delete infos[containerId];
  infos.erase(containerId);

  return Nothing();
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
