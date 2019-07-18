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

#include <process/async.hpp>

#include "common/future_tracker.hpp"

#include "slave/constants.hpp"

#include "slave/containerizer/mesos/launcher_tracker.hpp"

using std::map;
using std::string;
using std::vector;

using process::Future;
using process::Promise;

namespace mesos {
namespace internal {
namespace slave {

LauncherTracker::LauncherTracker(
    const process::Owned<Launcher>& _launcher, PendingFutureTracker* _tracker)
  : launcher(_launcher), tracker(_tracker)
{}


Future<hashset<ContainerID>> LauncherTracker::recover(
    const vector<mesos::slave::ContainerState>& states)
{
  return tracker->track(
      launcher->recover(states),
      "launcher::recover",
      COMPONENT_NAME_CONTAINERIZER);
}


Try<pid_t> LauncherTracker::fork(
    const ContainerID& containerId,
    const string& path,
    const vector<string>& argv,
    const mesos::slave::ContainerIO& containerIO,
    const flags::FlagsBase* flags,
    const Option<map<string, string>>& environment,
    const Option<int>& enterNamespaces,
    const Option<int>& cloneNamespaces,
    const vector<int_fd>& whitelistFds)
{
  Promise<Try<pid_t>> promise;

  tracker->track(
      promise.future(),
      "launcher::fork",
      COMPONENT_NAME_CONTAINERIZER,
      {{"containerId", stringify(containerId)},
       {"path", path}});

  Try<pid_t> forked = launcher->fork(
      containerId,
      path,
      argv,
      containerIO,
      flags,
      environment,
      enterNamespaces,
      cloneNamespaces,
      whitelistFds);

  promise.set(forked);
  return forked;
}


Future<Nothing> LauncherTracker::destroy(const ContainerID& containerId)
{
  return tracker->track(
      launcher->destroy(containerId),
      "launcher::destroy",
      COMPONENT_NAME_CONTAINERIZER,
      {{"containerId", stringify(containerId)}});
}


Future<ContainerStatus> LauncherTracker::status(const ContainerID& containerId)
{
  return tracker->track(
      launcher->status(containerId),
      "launcher::status",
      COMPONENT_NAME_CONTAINERIZER,
      {{"containerId", stringify(containerId)}});
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
