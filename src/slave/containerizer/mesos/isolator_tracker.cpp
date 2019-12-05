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

#include "common/future_tracker.hpp"

#include "slave/constants.hpp"

#include "slave/containerizer/mesos/isolator_tracker.hpp"

using std::string;
using std::vector;

using process::Future;
using process::Owned;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {

IsolatorTracker::IsolatorTracker(
    const Owned<mesos::slave::Isolator>& _isolator,
    const string& _isolatorName,
    PendingFutureTracker* _tracker)
  : isolator(_isolator),
    isolatorName(_isolatorName),
    tracker(_tracker)
{}


bool IsolatorTracker::supportsNesting()
{
  return isolator->supportsNesting();
}


bool IsolatorTracker::supportsStandalone()
{
  return isolator->supportsStandalone();
}


Future<Nothing> IsolatorTracker::recover(
    const vector<ContainerState>& state,
    const hashset<ContainerID>& orphans)
{
  return tracker->track(
      isolator->recover(state, orphans),
      strings::format("%s::recover", isolatorName).get(),
      COMPONENT_NAME_CONTAINERIZER);
}


Future<Option<ContainerLaunchInfo>> IsolatorTracker::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  return tracker->track(
      isolator->prepare(containerId, containerConfig),
      strings::format("%s::prepare", isolatorName).get(),
      COMPONENT_NAME_CONTAINERIZER,
      {{"containerId", stringify(containerId)}});
}


Future<Nothing> IsolatorTracker::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  return tracker->track(
      isolator->isolate(containerId, pid),
      strings::format("%s::isolate", isolatorName).get(),
      COMPONENT_NAME_CONTAINERIZER,
      {{"containerId", stringify(containerId)},
       {"pid", stringify(pid)}});
}


Future<ContainerLimitation> IsolatorTracker::watch(
    const ContainerID& containerId)
{
  // Do not track `watch` method, since it is supposed
  // to be pending as long as the container is running.
  return isolator->watch(containerId);
}


Future<Nothing> IsolatorTracker::update(
    const ContainerID& containerId,
    const Resources& resourceRequests,
    const google::protobuf::Map<string, Value::Scalar>& resourceLimits)
{
  return tracker->track(
      isolator->update(containerId, resourceRequests, resourceLimits),
      strings::format("%s::update", isolatorName).get(),
      COMPONENT_NAME_CONTAINERIZER,
      {{"containerId", stringify(containerId)},
       {"resourceRequests", stringify(resourceRequests)},
       {"resourceLimits", stringify(resourceLimits)}});
}


Future<ResourceStatistics> IsolatorTracker::usage(
    const ContainerID& containerId)
{
  return tracker->track(
      isolator->usage(containerId),
      strings::format("%s::usage", isolatorName).get(),
      COMPONENT_NAME_CONTAINERIZER,
      {{"containerId", stringify(containerId)}});
}


Future<ContainerStatus> IsolatorTracker::status(
    const ContainerID& containerId)
{
  return tracker->track(
      isolator->status(containerId),
      strings::format("%s::status", isolatorName).get(),
      COMPONENT_NAME_CONTAINERIZER,
      {{"containerId", stringify(containerId)}});
}


Future<Nothing> IsolatorTracker::cleanup(
    const ContainerID& containerId)
{
  return tracker->track(
      isolator->cleanup(containerId),
      strings::format("%s::cleanup", isolatorName).get(),
      COMPONENT_NAME_CONTAINERIZER,
      {{"containerId", stringify(containerId)}});
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
