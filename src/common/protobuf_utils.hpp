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

#include <mesos/mesos.hpp>

#include <mesos/maintenance/maintenance.hpp>

#include <mesos/master/master.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/time.hpp>

#include <stout/duration.hpp>
#include <stout/ip.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/uuid.hpp>

#include "messages/messages.hpp"

// Forward declaration (in lieu of an include).
namespace process {
struct UPID;
}

namespace mesos {
namespace internal {

namespace master {
// Forward declaration (in lieu of an include).
struct Slave;
} // namespace master {

namespace protobuf {

bool frameworkHasCapability(
    const FrameworkInfo& framework,
    FrameworkInfo::Capability::Type capability);


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
    const Option<UUID>& uuid,
    const std::string& message = "",
    const Option<TaskStatus::Reason>& reason = None(),
    const Option<ExecutorID>& executorId = None(),
    const Option<bool>& healthy = None(),
    const Option<CheckStatusInfo>& checkStatus = None(),
    const Option<Labels>& labels = None(),
    const Option<ContainerStatus>& containerStatus = None(),
    const Option<TimeInfo>& unreachableTime = None());


StatusUpdate createStatusUpdate(
    const FrameworkID& frameworkId,
    const TaskStatus& status,
    const Option<SlaveID>& slaveId);


// Helper function that creates a new task status from scratch with
// obligatory fields set.
TaskStatus createTaskStatus(
    const TaskID& taskId,
    const TaskState& state,
    const UUID& uuid,
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
    const UUID& uuid,
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


// Helper function that creates a MasterInfo from UPID.
MasterInfo createMasterInfo(const process::UPID& pid);


Label createLabel(
    const std::string& key,
    const Option<std::string>& value = None());


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


// Helper function that fills in a TimeInfo from the current time.
TimeInfo getCurrentTime();


// Helper function that creates a `FileInfo` from data returned by `stat()`.
FileInfo createFileInfo(const std::string& path, const struct stat& s);


ContainerID getRootContainerId(const ContainerID& containerId);

namespace slave {

// TODO(bmahler): Store the repeated field within this so that we
// don't drop unknown capabilities.
struct Capabilities
{
  Capabilities() = default;

  template <typename Iterable>
  Capabilities(const Iterable& capabilities)
  {
    foreach (const SlaveInfo::Capability& capability, capabilities) {
      switch (capability.type()) {
        case SlaveInfo::Capability::UNKNOWN:
          break;
        case SlaveInfo::Capability::MULTI_ROLE:
          multiRole = true;
          break;
        // If adding another case here be sure to update the
        // equality operator.
      }
    }
  }

  // See mesos.proto for the meaning of agent capabilities.
  bool multiRole = false;

  google::protobuf::RepeatedPtrField<SlaveInfo::Capability>
  toRepeatedPtrField() const
  {
    google::protobuf::RepeatedPtrField<SlaveInfo::Capability> result;
    if (multiRole) {
      result.Add()->set_type(SlaveInfo::Capability::MULTI_ROLE);
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
    const ContainerID& id,
    pid_t pid,
    const std::string& directory);

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


// Helper for creating an `Agent` response.
mesos::master::Response::GetAgents::Agent createAgentResponse(
    const mesos::internal::master::Slave& slave);


// Helper for creating an `AGENT_ADDED` event from a `Slave`.
mesos::master::Event createAgentAdded(
    const mesos::internal::master::Slave& slave);


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
  Capabilities(const Iterable& capabilities)
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
};


// Helper to get roles from FrameworkInfo based on the
// presence of the MULTI_ROLE capability.
std::set<std::string> getRoles(const FrameworkInfo& frameworkInfo);

} // namespace framework {

} // namespace protobuf {
} // namespace internal {
} // namespace mesos {

#endif // __PROTOBUF_UTILS_HPP__
