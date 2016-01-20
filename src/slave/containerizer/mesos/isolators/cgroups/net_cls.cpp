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

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/cgroups/net_cls.hpp"

using namespace process;

using std::string;
using std::list;

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
  return nullptr;
}


Future<Nothing> CgroupsNetClsIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
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
  return ResourceStatistics();
}


Future<Option<ContainerLaunchInfo>> CgroupsNetClsIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const ContainerConfig& containerConfig)
{
  return None();
}


Future<Nothing> CgroupsNetClsIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  return Nothing();
}


// The net_cls handles are labels and hence there are no limitations associated
// with them . This function would therefore always return a pending future
// since the limitation is never reached.
Future<ContainerLimitation> CgroupsNetClsIsolatorProcess::watch(
    const ContainerID& containerId)
{
  return Future<ContainerLimitation>();
}


Future<Nothing> CgroupsNetClsIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
