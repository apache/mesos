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

#ifndef __PROTOBUF_UTILS_HPP__
#define __PROTOBUF_UTILS_HPP__

#include <initializer_list>
#include <ostream>
#include <set>
#include <string>

#include <sys/stat.h>

#include <google/protobuf/map.h>

#include <mesos/mesos.hpp>

#include <mesos/maintenance/maintenance.hpp>

#include <mesos/master/master.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/time.hpp>

#include <stout/duration.hpp>
#include <stout/ip.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "master/registry.hpp"

#include "messages/messages.hpp"

// Forward declaration (in lieu of an include).
namespace process {
struct UPID;
}

namespace mesos {

class ObjectApprovers;

namespace internal {

namespace master {
// Forward declaration (in lieu of an include).
struct Framework;
struct Slave;
} // namespace master {

namespace protobuf {

// Modeled after WireFormatLite from protobuf, but to provide
// missing helpers.
class WireFormatLite2
{
public:
  // This is a wrapper to compute cached sizes before calling into
  // `WireFormatLite::WriteMessage`, which assumes that sizes are
  // already cached.
  static void WriteMessageWithoutCachedSizes(
      int field_number,
      const google::protobuf::MessageLite& value,
      google::protobuf::io::CodedOutputStream* output)
  {
    // Cache the sizes first.
    value.ByteSizeLong();

    google::protobuf::internal::WireFormatLite::WriteMessage(
        field_number, value, output);
  }
};


// Internal helper class for protobuf union validation.
class UnionValidator
{
public:
  UnionValidator(const google::protobuf::Descriptor*);
  Option<Error> validate(
      const int messageTypeNumber, const google::protobuf::Message&) const;

private:
  std::vector<std::pair<int, const google::protobuf::FieldDescriptor*>>
    unionFieldDescriptors_;
  const google::protobuf::EnumDescriptor* typeDescriptor_;
};

//
// A message is a "protobuf union" if, and only if,
// the following requirements are satisfied:
// 1. It has a required field named `type` of an enum type.
// 2. A member of this enum with a number (not index!) of 0
//    either is named "UNKNOWN" or does not exist.
// 3. For each other member of this enum there is an optional field
//    in the message with an exactly matching name in lowercase.
// (Being or not being a protobuf uinion depends on a message declaration only.)
//
// A "protobuf union" is valid if, and only if, all the message fields
// which correspond to members of this enum that do not matching the value
// of the `type` field, are not set.
// (Validity of the protobuf union depends on the message contents.
// Note that it does not depend on whether the matching field is set or not.)
//
// NOTE: If possible, oneof should be used in the new messages instead
// of the "protobuf union".
//
// This function returns None if the protobuf union is valid
// and Error otherwise.
// In case the ProtobufUnion is not a protobuf union,
// this function will abort the process on the first use.
template <class ProtobufUnion>
Option<Error> validateProtobufUnion(const ProtobufUnion& message)
{
  static const UnionValidator validator(ProtobufUnion::descriptor());
  return validator.validate(message.type(), message);
}


bool frameworkHasCapability(
    const FrameworkInfo& framework,
    FrameworkInfo::Capability::Type capability);


// Returns whether the task state is terminal. Terminal states
// mean that the resources are released and the task cannot
// transition back to a non-terminal state. Note that
// `TASK_UNREACHABLE` is not a terminal state, but still
// releases the resources.
bool isTerminalState(const TaskState& state);


// See TaskStatus for more information about these fields. Note
// that the 'uuid' must be provided for updates that need
// acknowledgement. Currently, all slave and executor generated
// updates require acknowledgement, whereas master generated
// and scheduler driver generated updates do not.
StatusUpdate createStatusUpdate(
    const FrameworkID& frameworkId,
    const Option<SlaveID>& slaveId,
    const TaskID& taskId,
    const TaskState& state,
    const TaskStatus::Source& source,
    const Option<id::UUID>& uuid,
    const std::string& message = "",
    const Option<TaskStatus::Reason>& reason = None(),
    const Option<ExecutorID>& executorId = None(),
    const Option<bool>& healthy = None(),
    const Option<CheckStatusInfo>& checkStatus = None(),
    const Option<Labels>& labels = None(),
    const Option<ContainerStatus>& containerStatus = None(),
    const Option<TimeInfo>& unreachableTime = None(),
    const Option<Resources>& limitedResources = None());


StatusUpdate createStatusUpdate(
    const FrameworkID& frameworkId,
    const TaskStatus& status,
    const Option<SlaveID>& slaveId);


// Helper function that creates a new task status from scratch with
// obligatory fields set.
TaskStatus createTaskStatus(
    const TaskID& taskId,
    const TaskState& state,
    const id::UUID& uuid,
    double timestamp);


// Helper function that creates a new task status from the given task
// status. Specific fields in `status` can be overridden in the new
// status by specifying the appropriate argument. Fields `task_id`,
// `slave_id`, `executor_id`, cannot be changed; while `timestamp`
// and `uuid` cannot be preserved.
//
// NOTE: A task status update may be used for guaranteed delivery of
// some task-related information, e.g., task's health update. In this
// case, it is often desirable to preserve specific fields from the
// previous status update to avoid shadowing information that was
// delivered previously.
TaskStatus createTaskStatus(
    TaskStatus status,
    const id::UUID& uuid,
    double timestamp,
    const Option<TaskState>& state = None(),
    const Option<std::string>& message = None(),
    const Option<TaskStatus::Source>& source = None(),
    const Option<TaskStatus::Reason>& reason = None(),
    const Option<std::string>& data = None(),
    const Option<bool>& healthy = None(),
    const Option<CheckStatusInfo>& checkStatus = None(),
    const Option<Labels>& labels = None(),
    const Option<ContainerStatus>& containerStatus = None(),
    const Option<TimeInfo>& unreachableTime = None());


Task createTask(
    const TaskInfo& task,
    const TaskState& state,
    const FrameworkID& frameworkId);


Option<bool> getTaskHealth(const Task& task);


Option<CheckStatusInfo> getTaskCheckStatus(const Task& task);


Option<ContainerStatus> getTaskContainerStatus(const Task& task);


bool isTerminalState(const OperationState& state);


OperationStatus createOperationStatus(
    const OperationState& state,
    const Option<OperationID>& operationId = None(),
    const Option<std::string>& message = None(),
    const Option<Resources>& convertedResources = None(),
    const Option<id::UUID>& statusUUID = None(),
    const Option<SlaveID>& slaveId = None(),
    const Option<ResourceProviderID>& resourceProviderId = None());


Operation createOperation(
    const Offer::Operation& info,
    const OperationStatus& latestStatus,
    const Option<FrameworkID>& frameworkId,
    const Option<SlaveID>& slaveId,
    const Option<UUID>& operationUUID = None());


UpdateOperationStatusMessage createUpdateOperationStatusMessage(
    const UUID& operationUUID,
    const OperationStatus& status,
    const Option<OperationStatus>& latestStatus = None(),
    const Option<FrameworkID>& frameworkId = None(),
    const Option<SlaveID>& slaveId = None());


// Create a `UUID`. If `uuid` is given it is used to initialize
// the created `UUID`; otherwise a random `UUID` is returned.
UUID createUUID(const Option<id::UUID>& uuid = None());


// Helper function that creates a MasterInfo from UPID.
MasterInfo createMasterInfo(const process::UPID& pid);


Label createLabel(
    const std::string& key,
    const Option<std::string>& value = None());


// Helper function to convert a protobuf string map to `Labels`.
Labels convertStringMapToLabels(
    const google::protobuf::Map<std::string, std::string>& map);


// Helper function to convert a `Labels` to a protobuf string map.
Try<google::protobuf::Map<std::string, std::string>> convertLabelsToStringMap(
    const Labels& labels);


// Previously, `Resource` did not contain `AllocationInfo`.
// So for backwards compatibility with old schedulers and
// tooling, we must allow operations to contain `Resource`s
// without an `AllocationInfo`. This allows the master to
// inject the offer's `AllocationInfo` into the operation's
// resources.
void injectAllocationInfo(
    Offer::Operation* operation,
    const Resource::AllocationInfo& allocationInfo);


// This strips the Resource::AllocationInfo from all
// Resource objects contained within the operation.
void stripAllocationInfo(Offer::Operation* operation);


bool isSpeculativeOperation(const Offer::Operation& operation);


// Helper function to pack a protobuf list of resource versions.
google::protobuf::RepeatedPtrField<ResourceVersionUUID> createResourceVersions(
    const hashmap<Option<ResourceProviderID>, UUID>& resourceVersions);


// Helper function to unpack a protobuf list of resource versions.
hashmap<Option<ResourceProviderID>, UUID> parseResourceVersions(
    const google::protobuf::RepeatedPtrField<ResourceVersionUUID>&
      resourceVersionUUIDs);


// Helper function that fills in a TimeInfo from the current time.
TimeInfo getCurrentTime();


// Helper function that creates a `FileInfo` from data returned by `stat()`.
FileInfo createFileInfo(const std::string& path, const struct stat& s);


ContainerID getRootContainerId(const ContainerID& containerId);


ContainerID parseContainerId(const std::string& value);


Try<Resources> getConsumedResources(const Offer::Operation& operation);

namespace slave {

// TODO(bmahler): Store the repeated field within this so that we
// don't drop unknown capabilities.
struct Capabilities
{
  Capabilities() = default;

  template <typename Iterable>
  explicit Capabilities(const Iterable& capabilities)
  {
    foreach (const SlaveInfo::Capability& capability, capabilities) {
      switch (capability.type()) {
        case SlaveInfo::Capability::UNKNOWN:
          break;
        case SlaveInfo::Capability::MULTI_ROLE:
          multiRole = true;
          break;
        case SlaveInfo::Capability::HIERARCHICAL_ROLE:
          hierarchicalRole = true;
          break;
        case SlaveInfo::Capability::RESERVATION_REFINEMENT:
          reservationRefinement = true;
          break;
        case SlaveInfo::Capability::RESOURCE_PROVIDER:
          resourceProvider = true;
          break;
        case SlaveInfo::Capability::RESIZE_VOLUME:
          resizeVolume = true;
          break;
        case SlaveInfo::Capability::AGENT_OPERATION_FEEDBACK:
          agentOperationFeedback = true;
          break;
        case SlaveInfo::Capability::AGENT_DRAINING:
          agentDraining = true;
          break;
        case SlaveInfo::Capability::TASK_RESOURCE_LIMITS:
          taskResourceLimits = true;
          break;
        // If adding another case here be sure to update the
        // equality operator.
      }
    }
  }

  // See mesos.proto for the meaning of agent capabilities.
  bool multiRole = false;
  bool hierarchicalRole = false;
  bool reservationRefinement = false;
  bool resourceProvider = false;
  bool resizeVolume = false;
  bool agentOperationFeedback = false;
  bool agentDraining = false;
  bool taskResourceLimits = false;

  google::protobuf::RepeatedPtrField<SlaveInfo::Capability>
  toRepeatedPtrField() const
  {
    google::protobuf::RepeatedPtrField<SlaveInfo::Capability> result;
    if (multiRole) {
      result.Add()->set_type(SlaveInfo::Capability::MULTI_ROLE);
    }
    if (hierarchicalRole) {
      result.Add()->set_type(SlaveInfo::Capability::HIERARCHICAL_ROLE);
    }
    if (reservationRefinement) {
      result.Add()->set_type(SlaveInfo::Capability::RESERVATION_REFINEMENT);
    }
    if (resourceProvider) {
      result.Add()->set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
    }
    if (resizeVolume) {
      result.Add()->set_type(SlaveInfo::Capability::RESIZE_VOLUME);
    }
    if (agentOperationFeedback) {
      result.Add()->set_type(SlaveInfo::Capability::AGENT_OPERATION_FEEDBACK);
    }
    if (agentDraining) {
      result.Add()->set_type(SlaveInfo::Capability::AGENT_DRAINING);
    }
    if (taskResourceLimits) {
      result.Add()->set_type(SlaveInfo::Capability::TASK_RESOURCE_LIMITS);
    }

    return result;
  }
};


bool operator==(const Capabilities& left, const Capabilities& right);
bool operator!=(const Capabilities& left, const Capabilities& right);
std::ostream& operator<<(std::ostream& stream, const Capabilities& c);


mesos::slave::ContainerLimitation createContainerLimitation(
    const Resources& resources,
    const std::string& message,
    const TaskStatus::Reason& reason);


mesos::slave::ContainerState createContainerState(
    const Option<ExecutorInfo>& executorInfo,
    const Option<ContainerInfo>& containerInfo,
    const ContainerID& id,
    pid_t pid,
    const std::string& directory);


mesos::slave::ContainerMountInfo createContainerMount(
    const std::string& source,
    const std::string& target,
    unsigned long flags);


mesos::slave::ContainerMountInfo createContainerMount(
    const std::string& source,
    const std::string& target,
    const std::string& type,
    unsigned long flags);


mesos::slave::ContainerMountInfo createContainerMount(
    const std::string& source,
    const std::string& target,
    const std::string& type,
    const std::string& options,
    unsigned long flags);


mesos::slave::ContainerFileOperation containerSymlinkOperation(
    const std::string& source,
    const std::string& target);


mesos::slave::ContainerFileOperation containerRenameOperation(
    const std::string& source,
    const std::string& target);


mesos::slave::ContainerFileOperation containerMkdirOperation(
    const std::string& target,
    const bool recursive);


mesos::slave::ContainerFileOperation containerMountOperation(
    const mesos::slave::ContainerMountInfo& mnt);

} // namespace slave {

namespace maintenance {

/**
 * Helper for constructing an unavailability from a `Time` and `Duration`.
 */
Unavailability createUnavailability(
    const process::Time& start,
    const Option<Duration>& duration = None());


/**
 * Helper for constructing a list of `MachineID`.
 */
google::protobuf::RepeatedPtrField<MachineID> createMachineList(
    std::initializer_list<MachineID> ids);


/**
 * Helper for constructing a maintenance `Window`.
 * See `createUnavailability` above.
 */
mesos::maintenance::Window createWindow(
    std::initializer_list<MachineID> ids,
    const Unavailability& unavailability);


/**
 * Helper for constructing a maintenance `Schedule`.
 * See `createWindow` above.
 */
mesos::maintenance::Schedule createSchedule(
    std::initializer_list<mesos::maintenance::Window> windows);

} // namespace maintenance {


namespace master {

// TODO(mzhu): Consolidate these helpers into `struct Capabilities`.
// For example, to add a minimum capability for `QUOTA_V2`, we could do the
// following in the call site:
//
//  Capabilities capabilities = registry->minimum_capabilities();
//  capabilities.quotaV2 = needsV2;
//  *registry->mutable_minimum_capabilities() = capabilities.toStrings();
//
// For this to work, we need to:
//  - Add a constructor from repeated `MinimumCapability`
//  - Add a toStrings() that goes back to repeated string
//  - Note, unknown capabilities need to be carried in the struct.
//
// In addition, we should consolidate the helper
// `Master::misingMinimumCapabilities` into the struct as well.

// Helper to add a minimum capability, it is a noop if already set.
void addMinimumCapability(
    google::protobuf::RepeatedPtrField<Registry::MinimumCapability>*
      capabilities,
    const MasterInfo::Capability::Type& capability);


// Helper to remove a minimum capability,
// it is a noop if already absent.
void removeMinimumCapability(
    google::protobuf::RepeatedPtrField<Registry::MinimumCapability>*
      capabilities,
    const MasterInfo::Capability::Type& capability);


// TODO(bmahler): Store the repeated field within this so that we
// don't drop unknown capabilities.
struct Capabilities
{
  Capabilities() = default;

  template <typename Iterable>
  explicit Capabilities(const Iterable& capabilities)
  {
    foreach (const MasterInfo::Capability& capability, capabilities) {
      switch (capability.type()) {
        case MasterInfo::Capability::UNKNOWN:
          break;
        case MasterInfo::Capability::AGENT_UPDATE:
          agentUpdate = true;
          break;
        case MasterInfo::Capability::AGENT_DRAINING:
          agentDraining = true;
          break;
        case MasterInfo::Capability::QUOTA_V2:
          quotaV2 = true;
          break;
      }
    }
  }

  bool agentUpdate = false;
  bool agentDraining = false;
  bool quotaV2 = false;
};

namespace event {

// Helper for creating a `TASK_UPDATED` event from a `Task`, its
// latest state according to the agent, and its status corresponding
// to the last status update acknowledged from the scheduler.
mesos::master::Event createTaskUpdated(
    const Task& task,
    const TaskState& state,
    const TaskStatus& status);


// Helper for creating a `TASK_ADDED` event from a `Task`.
mesos::master::Event createTaskAdded(const Task& task);


// Helper for creating a 'FRAMEWORK_ADDED' event from a `Framework`.
mesos::master::Event createFrameworkAdded(
    const mesos::internal::master::Framework& framework);


// Helper for creating a 'FRAMEWORK_UPDATED' event from a `Framework`.
mesos::master::Event createFrameworkUpdated(
    const mesos::internal::master::Framework& framework);


// Helper for creating a 'FRAMEWORK_REMOVED' event from a `FrameworkInfo`.
mesos::master::Event createFrameworkRemoved(const FrameworkInfo& frameworkInfo);


// Helper for creating an `Agent` response.
mesos::master::Response::GetAgents::Agent createAgentResponse(
    const mesos::internal::master::Slave& slave,
    const Option<DrainInfo>& drainInfo,
    bool deactivated,
    const Option<process::Owned<ObjectApprovers>>& approvers = None());


// Helper for creating an `AGENT_ADDED` event from a `Slave`.
mesos::master::Event createAgentAdded(
    const mesos::internal::master::Slave& slave,
    const Option<DrainInfo>& drainInfo,
    bool deactivated);


// Helper for creating an `AGENT_REMOVED` event from a `SlaveID`.
mesos::master::Event createAgentRemoved(const SlaveID& slaveId);

} // namespace event {
} // namespace master {

namespace framework {

// TODO(bmahler): Store the repeated field within this so that we
// don't drop unknown capabilities.
struct Capabilities
{
  Capabilities() = default;

  template <typename Iterable>
  explicit Capabilities(const Iterable& capabilities)
  {
    foreach (const FrameworkInfo::Capability& capability, capabilities) {
      switch (capability.type()) {
        case FrameworkInfo::Capability::UNKNOWN:
          break;
        case FrameworkInfo::Capability::REVOCABLE_RESOURCES:
          revocableResources = true;
          break;
        case FrameworkInfo::Capability::TASK_KILLING_STATE:
          taskKillingState = true;
          break;
        case FrameworkInfo::Capability::GPU_RESOURCES:
          gpuResources = true;
          break;
        case FrameworkInfo::Capability::SHARED_RESOURCES:
          sharedResources = true;
          break;
        case FrameworkInfo::Capability::PARTITION_AWARE:
          partitionAware = true;
          break;
        case FrameworkInfo::Capability::MULTI_ROLE:
          multiRole = true;
          break;
        case FrameworkInfo::Capability::RESERVATION_REFINEMENT:
          reservationRefinement = true;
          break;
        case FrameworkInfo::Capability::REGION_AWARE:
          regionAware = true;
          break;
      }
    }
  }

  // See mesos.proto for the meaning of these capabilities.
  bool revocableResources = false;
  bool taskKillingState = false;
  bool gpuResources = false;
  bool sharedResources = false;
  bool partitionAware = false;
  bool multiRole = false;
  bool reservationRefinement = false;
  bool regionAware = false;
};


// Helper to get roles from FrameworkInfo based on the
// presence of the MULTI_ROLE capability.
std::set<std::string> getRoles(const FrameworkInfo& frameworkInfo);

} // namespace framework {

} // namespace protobuf {
} // namespace internal {
} // namespace mesos {

#endif // __PROTOBUF_UTILS_HPP__
