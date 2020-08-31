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

#include <google/protobuf/util/message_differencer.h>

#include <stout/protobuf.hpp>
#include <stout/uuid.hpp>

#include <mesos/v1/attributes.hpp>
#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>

#include "common/type_utils_differencers.hpp"

using std::ostream;
using std::string;
using std::vector;
using std::unique_ptr;

using ::mesos::typeutils::internal::createFrameworkInfoDifferencer;

namespace mesos {
namespace v1 {

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
  // NOTE: We purposefully do not compare the value of the `cache` field
  // because a URI downloaded from source or from the fetcher cache should
  // be considered identical.
  return left.value() == right.value() &&
    left.executable() == right.executable() &&
    left.extract() == right.extract() &&
    left.output_file() == right.output_file();
}


bool operator==(const Credential& left, const Credential& right)
{
  return left.principal() == right.principal() &&
    left.secret() == right.secret();
}


bool operator==(const CSIPluginInfo& left, const CSIPluginInfo& right)
{
  // Order of containers is important.
  if (left.containers_size() != right.containers_size()) {
    return false;
  }

  for (int i = 0; i < left.containers_size(); i++) {
    if (left.containers(i) != right.containers(i)) {
      return false;
    }
  }

  return left.type() == right.type() &&
    left.name() == right.name();
}


bool operator==(
    const CSIPluginContainerInfo& left,
    const CSIPluginContainerInfo& right)
{
  // Order of services is not important.
  if (left.services_size() != right.services_size()) {
    return false;
  }

  vector<bool> used(right.services_size(), false);

  for (int i = 0; i < left.services_size(); i++) {
    bool found = false;
    for (int j = 0; j < right.services_size(); j++) {
      if (left.services(i) == right.services(j) && !used[j]) {
        found = used[j] = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }

  return left.has_command() == right.has_command() &&
    (!left.has_command() || left.command() == right.command()) &&
    Resources(left.resources()) == Resources(right.resources()) &&
    left.has_container() == right.has_container() &&
    (!left.has_container() || left.container() == right.container());
}


bool operator==(const DrainInfo& left, const DrainInfo& right)
{
  return google::protobuf::util::MessageDifferencer::Equals(left, right);
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
  return left.executor_id() == right.executor_id() &&
    left.data() == right.data() &&
    Resources(left.resources()) == Resources(right.resources()) &&
    left.command() == right.command() &&
    left.framework_id() == right.framework_id() &&
    left.name() == right.name() &&
    left.source() == right.source() &&
    left.container() == right.container() &&
    left.discovery() == right.discovery();
}


bool operator==(const FileInfo& left, const FileInfo& right)
{
  return left.path() == right.path() &&
    left.nlink() == right.nlink() &&
    left.size() == right.size() &&
    left.mtime() == right.mtime() &&
    left.mode() == right.mode() &&
    left.uid() == right.uid() &&
    left.gid() == right.gid();
}


bool operator==(const MasterInfo& left, const MasterInfo& right)
{
  return left.id() == right.id() &&
    left.ip() == right.ip() &&
    left.port() == right.port() &&
    left.pid() == right.pid() &&
    left.hostname() == right.hostname() &&
    left.version() == right.version() &&
    left.domain() == right.domain();
}


bool operator==(const Offer::Operation& left, const Offer::Operation& right)
{
  return google::protobuf::util::MessageDifferencer::Equals(left, right);
}


bool operator==(const Operation& left, const Operation& right)
{
  return google::protobuf::util::MessageDifferencer::Equals(left, right);
}


bool operator!=(const Offer::Operation& left, const Offer::Operation& right)
{
  return !(left == right);
}


bool operator!=(const Operation& left, const Operation& right)
{
  return !(left == right);
}


bool operator==(
    const ResourceProviderInfo::Storage& left,
    const ResourceProviderInfo::Storage& right)
{
  return left.plugin() == right.plugin();
}


bool operator==(
    const ResourceProviderInfo& left,
    const ResourceProviderInfo& right)
{
  // Order of reservations is important.
  if (left.default_reservations_size() != right.default_reservations_size()) {
    return false;
  }

  for (int i = 0; i < left.default_reservations_size(); i++) {
    if (left.default_reservations(i) != right.default_reservations(i)) {
      return false;
    }
  }

  return left.has_id() == right.has_id() &&
    (!left.has_id() || left.id() == right.id()) &&
    Attributes(left.attributes()) == Attributes(right.attributes()) &&
    left.type() == right.type() &&
    left.name() == right.name() &&
    left.has_storage() == right.has_storage() &&
    (!left.has_storage() || left.storage() == right.storage());
}


bool operator==(
    const ResourceStatistics& left,
    const ResourceStatistics& right)
{
  return left.SerializeAsString() == right.SerializeAsString();
}


bool operator==(const AgentInfo& left, const AgentInfo& right)
{
  return left.hostname() == right.hostname() &&
    Resources(left.resources()) == Resources(right.resources()) &&
    Attributes(left.attributes()) == Attributes(right.attributes()) &&
    left.id() == right.id() &&
    left.port() == right.port() &&
    left.domain() == right.domain();
}


// TODO(bmahler): Use SerializeToString here?
bool operator==(const TaskStatus& left, const TaskStatus& right)
{
  return left.task_id() == right.task_id() &&
    left.state() == right.state() &&
    left.data() == right.data() &&
    left.message() == right.message() &&
    left.agent_id() == right.agent_id() &&
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


namespace typeutils {

bool equivalent(const FrameworkInfo& left, const FrameworkInfo& right)
{
  return createFrameworkInfoDifferencer<FrameworkInfo>()->Compare(left, right);
}


Option<string> diff(const FrameworkInfo& left, const FrameworkInfo& right)
{
  unique_ptr<::google::protobuf::util::MessageDifferencer> differencer{
    createFrameworkInfoDifferencer<FrameworkInfo>()};

  string result;
  differencer->ReportDifferencesToString(&result);
  if (differencer->Compare(left, right)) {
    return None();
  }

  return result;
}

} // namespace typeutils {


ostream& operator<<(ostream& stream, const CapabilityInfo& capabilityInfo)
{
  return stream << JSON::protobuf(capabilityInfo);
}


ostream& operator<<(ostream& stream, const CheckStatusInfo& checkStatusInfo)
{
  switch (checkStatusInfo.type()) {
    case CheckInfo::COMMAND:
      if (checkStatusInfo.has_command()) {
        stream << "COMMAND";
        if (checkStatusInfo.command().has_exit_code()) {
          stream << " exit code " << checkStatusInfo.command().exit_code();
        }
      }
      break;
    case CheckInfo::HTTP:
      if (checkStatusInfo.has_http()) {
        stream << "HTTP";
        if (checkStatusInfo.http().has_status_code()) {
          stream << " status code " << checkStatusInfo.http().status_code();
        }
      }
      break;
    case CheckInfo::TCP:
      if (checkStatusInfo.has_tcp()) {
        stream << "TCP";
        if (checkStatusInfo.tcp().has_succeeded()) {
          stream << (checkStatusInfo.tcp().succeeded() ? " connection success"
                                                       : " connection failure");
        }
      }
      break;
    case CheckInfo::UNKNOWN:
      stream << "UNKNOWN";
      break;
  }

  return stream;
}


ostream& operator<<(ostream& stream, const ContainerID& containerId)
{
  return stream << containerId.value();
}


ostream& operator<<(ostream& stream, const ContainerInfo& containerInfo)
{
  return stream << containerInfo.DebugString();
}


ostream& operator<<(ostream& stream, const DomainInfo& domainInfo)
{
  return stream << JSON::protobuf(domainInfo);
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


ostream& operator<<(ostream& stream, const OperationID& operationId)
{
  return stream << operationId.value();
}


ostream& operator<<(ostream& stream, const OperationState& state)
{
  return stream << OperationState_Name(state);
}


ostream& operator<<(ostream& stream, const RateLimits& limits)
{
  return stream << limits.DebugString();
}


ostream& operator<<(
    ostream& stream,
    const ResourceProviderID& resourceProviderId)
{
  return stream << resourceProviderId.value();
}


ostream& operator<<(
    ostream& stream,
    const ResourceProviderInfo& resourceProviderInfo)
{
  return stream << JSON::protobuf(resourceProviderInfo);
}


ostream& operator<<(ostream& stream, const RLimitInfo& limits)
{
  return stream << JSON::protobuf(limits);
}


ostream& operator<<(ostream& stream, const TaskStatus& status)
{
  stream << status.state();

  if (status.has_uuid()) {
    stream << " (Status UUID: "
           << stringify(id::UUID::fromBytes(status.uuid()).get()) << ")";
  }

  if (status.has_source()) {
    stream << " Source: " << TaskStatus::Source_Name(status.source());
  }

  if (status.has_reason()) {
    stream << " Reason: " << TaskStatus::Reason_Name(status.reason());
  }

  if (status.has_message()) {
    stream << " Message: '" << status.message() << "'";
  }

  stream << " for task '" << status.task_id() << "'";

  if (status.has_agent_id()) {
    stream << " on agent: " << status.agent_id() << "";
  }

  if (status.has_healthy()) {
    stream << " in health state "
           << (status.healthy() ? "healthy" : "unhealthy");
  }

  return stream;
}


ostream& operator<<(ostream& stream, const OperationStatus& status)
{
  stream << status.state();

  if (status.has_uuid()) {
    stream << " (Status UUID: "
           << stringify(id::UUID::fromBytes(status.uuid().value()).get())
           << ")";
  }

  if (status.has_message()) {
    stream << " Message: '" << status.message() << "'";
  }

  if (status.has_operation_id()) {
    stream << " for operation '" << status.operation_id() << "'";
  }

  if (status.has_agent_id()) {
    stream << " on agent: " << status.agent_id() << "";
  }

  if (status.has_resource_provider_id()) {
    stream << " on resource provider: " << status.resource_provider_id() << "";
  }

  return stream;
}


ostream& operator<<(ostream& stream, const AgentID& agentId)
{
  return stream << agentId.value();
}


ostream& operator<<(ostream& stream, const AgentInfo& agent)
{
  return stream << agent.DebugString();
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


ostream& operator<<(ostream& stream, const TaskGroupInfo& taskGroupInfo)
{
  return stream << taskGroupInfo.DebugString();
}


ostream& operator<<(ostream& stream, const TaskState& state)
{
  return stream << TaskState_Name(state);
}


ostream& operator<<(ostream& stream, const CheckInfo::Type& type)
{
  return stream << CheckInfo::Type_Name(type);
}


ostream& operator<<(
    ostream& stream,
    const CSIPluginContainerInfo::Service& service)
{
  return stream << CSIPluginContainerInfo::Service_Name(service);
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


ostream& operator<<(ostream& stream, const Secret::Type& secretType)
{
  return stream << Secret::Type_Name(secretType);
}


ostream& operator<<(
    ostream& stream,
    const Offer::Operation::Type& operationType)
{
  return stream << Offer::Operation::Type_Name(operationType);
}


ostream& operator<<(
    ostream& stream,
    const Resource::DiskInfo::Source::Type& sourceType)
{
  return stream << Resource::DiskInfo::Source::Type_Name(sourceType);
}

} // namespace v1 {
} // namespace mesos {
