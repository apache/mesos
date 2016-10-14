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

#include <ostream>

#include <mesos/attributes.hpp>
#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <stout/protobuf.hpp>

#include "messages/messages.hpp"

using std::ostream;
using std::string;
using std::vector;

namespace mesos {

// TODO(vinod): Ensure that these operators do not go out of sync
// when new fields are added to the protobufs (MESOS-2487).

bool operator==(const CommandInfo& left, const CommandInfo& right)
{
  if (left.uris().size() != right.uris().size()) {
    return false;
  }

  // TODO(vinod): Factor out the comparison for repeated fields.
  for (int i = 0; i < left.uris().size(); i++) {
    bool found = false;
    for (int j = 0; j < right.uris().size(); j++) {
      if (left.uris().Get(i) == right.uris().Get(j)) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  if (left.arguments().size() != right.arguments().size()) {
    return false;
  }

  // The order of argv is important.
  for (int i = 0; i < left.arguments().size(); i++) {
    if (left.arguments().Get(i) != right.arguments().Get(i)) {
      return false;
    }
  }

  // NOTE: We are not validating CommandInfo::ContainerInfo here
  // because it is being deprecated in favor of ContainerInfo.
  // TODO(vinod): Kill the above comment when
  // CommandInfo::ContainerInfo is removed.
  return left.environment() == right.environment() &&
    left.value() == right.value() &&
    left.user() == right.user() &&
    left.shell() == right.shell();
}


bool operator==(const CommandInfo::URI& left, const CommandInfo::URI& right)
{
  return left.value() == right.value() &&
    left.executable() == right.executable() &&
    left.extract() == right.extract();
}


bool operator==(const ContainerID& left, const ContainerID& right)
{
  return left.value() == right.value() &&
    left.has_parent() == right.has_parent() &&
    (!left.has_parent() || left.parent() == right.parent());
}


bool operator==(const Credential& left, const Credential& right)
{
  return left.principal() == right.principal() &&
    left.secret() == right.secret();
}


bool operator==(
    const Environment::Variable& left,
    const Environment::Variable& right)
{
  return left.name() == right.name() && left.value() == right.value();
}


bool operator==(const Environment& left, const Environment& right)
{
  // Order of variables is not important.
  if (left.variables().size() != right.variables().size()) {
    return false;
  }

  for (int i = 0; i < left.variables().size(); i++) {
    bool found = false;
    for (int j = 0; j < right.variables().size(); j++) {
      if (left.variables().Get(i) == right.variables().Get(j)) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  return true;
}


bool operator==(const Volume& left, const Volume& right)
{
  return left.container_path() == right.container_path() &&
    left.host_path() == right.host_path() &&
    left.mode() == right.mode();
}


// TODO(bmahler): Leverage process::http::URL for equality.
bool operator==(const URL& left, const URL& right)
{
  return left.SerializeAsString() == right.SerializeAsString();
}


bool operator==(
    const ContainerInfo::DockerInfo::PortMapping& left,
    const ContainerInfo::DockerInfo::PortMapping& right)
{
  return left.host_port() == right.host_port() &&
    left.container_port() == right.container_port() &&
    left.protocol() == right.protocol();
}


bool operator==(const Parameter& left, const Parameter& right)
{
  return left.key() == right.key() && left.value() == right.value();
}


bool operator==(
    const ContainerInfo::DockerInfo& left,
    const ContainerInfo::DockerInfo& right)
{
  // Order of port mappings is not important.
  if (left.port_mappings().size() != right.port_mappings().size()) {
    return false;
  }

  for (int i = 0; i < left.port_mappings().size(); i++) {
    bool found = false;
    for (int j = 0; j < right.port_mappings().size(); j++) {
      if (left.port_mappings().Get(i) == right.port_mappings().Get(j)) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  // Order of parameters is not important.
  if (left.parameters().size() != right.parameters().size()) {
    return false;
  }

  for (int i = 0; i < left.parameters().size(); i++) {
    bool found = false;
    for (int j = 0; j < right.parameters().size(); j++) {
      if (left.parameters().Get(i) == right.parameters().Get(j)) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  return left.image() == right.image() &&
    left.network() == right.network() &&
    left.privileged() == right.privileged() &&
    left.force_pull_image() == right.force_pull_image();
}


bool operator==(const ContainerInfo& left, const ContainerInfo& right)
{
  // Order of volumes is not important.
  if (left.volumes().size() != right.volumes().size()) {
    return false;
  }

  for (int i = 0; i < left.volumes().size(); i++) {
    bool found = false;
    for (int j = 0; j < right.volumes().size(); j++) {
      if (left.volumes().Get(i) == right.volumes().Get(j)) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  return left.type() == right.type() &&
    left.hostname() == right.hostname() &&
    left.docker() == right.docker();
}


bool operator==(const Port& left, const Port& right)
{
  return left.number() == right.number() &&
    left.name() == right.name() &&
    left.protocol() == right.protocol() &&
    left.visibility() == right.visibility();
}


bool operator==(const Ports& left, const Ports& right)
{
  // Order of ports is not important.
  if (left.ports().size() != right.ports().size()) {
    return false;
  }

  for (int i = 0; i < left.ports().size(); i++) {
    bool found = false;
    for (int j = 0; j < right.ports().size(); j++) {
      if (left.ports().Get(i) == right.ports().Get(j)) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  return true;
}


bool operator==(const Label& left, const Label& right)
{
  return left.key() == right.key() && left.value() == right.value();
}


bool operator==(const Labels& left, const Labels& right)
{
  // Order of labels is not important.
  if (left.labels().size() != right.labels().size()) {
    return false;
  }

  for (int i = 0; i < left.labels().size(); i++) {
    bool found = false;
    for (int j = 0; j < right.labels().size(); j++) {
      if (left.labels().Get(i) == right.labels().Get(j)) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  return true;
}


bool operator!=(const Labels& left, const Labels& right)
{
  return !(left == right);
}


bool operator==(const DiscoveryInfo& left, const DiscoveryInfo& right)
{
  return left.visibility() == right.visibility() &&
    left.name() == right.name() &&
    left.environment() == right.environment() &&
    left.location() == right.location() &&
    left.version() == right.version() &&
    left.ports() == right.ports() &&
    left.labels() == right.labels();
}


bool operator==(const ExecutorInfo& left, const ExecutorInfo& right)
{
  if (left.has_type() && right.has_type()) {
    if (left.type() != right.type()) {
      return false;
    }
  }

  return left.has_type() == right.has_type() &&
    left.executor_id() == right.executor_id() &&
    left.data() == right.data() &&
    Resources(left.resources()) == Resources(right.resources()) &&
    left.command() == right.command() &&
    left.framework_id() == right.framework_id() &&
    left.name() == right.name() &&
    left.source() == right.source() &&
    left.container() == right.container() &&
    left.discovery() == right.discovery();
}


bool operator!=(const ExecutorInfo& left, const ExecutorInfo& right)
{
  return !(left == right);
}


bool operator==(const MasterInfo& left, const MasterInfo& right)
{
  return left.id() == right.id() &&
    left.ip() == right.ip() &&
    left.port() == right.port() &&
    left.pid() == right.pid() &&
    left.hostname() == right.hostname() &&
    left.version() == right.version();
}


bool operator==(
    const ResourceStatistics& left,
    const ResourceStatistics& right)
{
  return left.SerializeAsString() == right.SerializeAsString();
}


bool operator==(const SlaveInfo& left, const SlaveInfo& right)
{
  return left.hostname() == right.hostname() &&
    Resources(left.resources()) == Resources(right.resources()) &&
    Attributes(left.attributes()) == Attributes(right.attributes()) &&
    left.id() == right.id() &&
    left.checkpoint() == right.checkpoint() &&
    left.port() == right.port();
}


bool operator==(const Task& left, const Task& right)
{
  // Order of task statuses is important.
  if (left.statuses().size() != right.statuses().size()) {
    return false;
  }

  for (int i = 0; i < left.statuses().size(); i++) {
    if (left.statuses().Get(i) != right.statuses().Get(i)) {
      return false;
    }
  }

  return left.name() == right.name() &&
    left.task_id() == right.task_id() &&
    left.framework_id() == right.framework_id() &&
    left.executor_id() == right.executor_id() &&
    left.slave_id() == right.slave_id() &&
    left.state() == right.state() &&
    Resources(left.resources()) == Resources(right.resources()) &&
    left.status_update_state() == right.status_update_state() &&
    left.status_update_uuid() == right.status_update_uuid() &&
    left.labels() == right.labels() &&
    left.discovery() == right.discovery() &&
    left.user() == right.user();
}


bool operator==(const TaskGroupInfo& left, const TaskGroupInfo& right)
{
  // Order of tasks in a task group is not important.
  if (left.tasks().size() != right.tasks().size()) {
    return false;
  }

  for (int i = 0; i < left.tasks().size(); i++) {
    bool found = false;
    for (int j = 0; j < right.tasks().size(); j++) {
      if (left.tasks().Get(i) == right.tasks().Get(j)) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  return true;
}


// TODO(anand): Consider doing a field by field comparison instead.
bool operator==(const TaskInfo& left, const TaskInfo& right)
{
  return left.SerializeAsString() == right.SerializeAsString();
}


// TODO(bmahler): Use SerializeToString here?
bool operator==(const TaskStatus& left, const TaskStatus& right)
{
  return left.task_id() == right.task_id() &&
    left.state() == right.state() &&
    left.data() == right.data() &&
    left.message() == right.message() &&
    left.slave_id() == right.slave_id() &&
    left.timestamp() == right.timestamp() &&
    left.executor_id() == right.executor_id() &&
    left.healthy() == right.healthy() &&
    left.source() == right.source() &&
    left.reason() == right.reason() &&
    left.uuid() == right.uuid();
}


bool operator!=(const TaskStatus& left, const TaskStatus& right)
{
  return !(left == right);
}


ostream& operator<<(std::ostream& stream, const CapabilityInfo& capabilityInfo)
{
  return stream << JSON::protobuf(capabilityInfo);
}


ostream& operator<<(ostream& stream, const CommandInfo& commandInfo)
{
  return stream << JSON::protobuf(commandInfo);
}


ostream& operator<<(ostream& stream, const ContainerID& containerId)
{
  return containerId.has_parent()
    ? stream << containerId.parent() << "." << containerId.value()
    : stream << containerId.value();
}


ostream& operator<<(ostream& stream, const ContainerInfo& containerInfo)
{
  return stream << containerInfo.DebugString();
}


ostream& operator<<(ostream& stream, const ExecutorID& executorId)
{
  return stream << executorId.value();
}


ostream& operator<<(ostream& stream, const ExecutorInfo& executor)
{
  return stream << executor.DebugString();
}


ostream& operator<<(ostream& stream, const FrameworkID& frameworkId)
{
  return stream << frameworkId.value();
}


ostream& operator<<(ostream& stream, const MasterInfo& master)
{
  return stream << master.DebugString();
}


ostream& operator<<(ostream& stream, const OfferID& offerId)
{
  return stream << offerId.value();
}


ostream& operator<<(ostream& stream, const RateLimits& limits)
{
  return stream << limits.DebugString();
}


ostream& operator<<(ostream& stream, const SlaveID& slaveId)
{
  return stream << slaveId.value();
}


ostream& operator<<(ostream& stream, const SlaveInfo& slave)
{
  return stream << slave.DebugString();
}


ostream& operator<<(ostream& stream, const TaskID& taskId)
{
  return stream << taskId.value();
}


ostream& operator<<(ostream& stream, const MachineID& machineId)
{
  if (machineId.has_hostname() && machineId.has_ip()) {
    return stream << machineId.hostname() << " (" << machineId.ip() << ")";
  }

  // If only a hostname is present.
  if (machineId.has_hostname()) {
    return stream << machineId.hostname();
  } else { // If there is no hostname, then there is an IP.
    return stream << "(" << machineId.ip() << ")";
  }
}


ostream& operator<<(ostream& stream, const TaskInfo& task)
{
  return stream << task.DebugString();
}


ostream& operator<<(ostream& stream, const TaskState& state)
{
  return stream << TaskState_Name(state);
}


ostream& operator<<(ostream& stream, const vector<TaskID>& taskIds)
{
  stream << "[ ";
  for (auto it = taskIds.begin(); it != taskIds.end(); ++it) {
    if (it != taskIds.begin()) {
      stream << ", ";
    }
    stream << *it;
  }
  stream << " ]";
  return stream;
}


ostream& operator<<(
    ostream& stream,
    const FrameworkInfo::Capability& capability)
{
  return stream << FrameworkInfo::Capability::Type_Name(capability.type());
}


ostream& operator<<(ostream& stream, const Image::Type& imageType)
{
  return stream << Image::Type_Name(imageType);
}


ostream& operator<<(ostream& stream, const hashmap<string, string>& map)
{
  return stream << stringify(map);
}

} // namespace mesos {
