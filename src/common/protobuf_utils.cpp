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

#include <mesos/type_utils.hpp>

#include <process/clock.hpp>
#include <process/pid.hpp>

#include <stout/net.hpp>
#include <stout/stringify.hpp>
#include <stout/uuid.hpp>

#include "messages/messages.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace protobuf {

bool isTerminalState(const TaskState& state)
{
  return (state == TASK_FINISHED ||
          state == TASK_FAILED ||
          state == TASK_KILLED ||
          state == TASK_LOST ||
          state == TASK_ERROR);
}


StatusUpdate createStatusUpdate(
    const FrameworkID& frameworkId,
    const Option<SlaveID>& slaveId,
    const TaskID& taskId,
    const TaskState& state,
    const TaskStatus::Source& source,
    const Option<UUID>& uuid,
    const string& message = "",
    const Option<TaskStatus::Reason>& reason = None(),
    const Option<ExecutorID>& executorId = None(),
    const Option<bool>& healthy = None(),
    const Option<Labels>& labels = None())
{
  StatusUpdate update;

  update.set_timestamp(process::Clock::now().secs());
  update.mutable_framework_id()->MergeFrom(frameworkId);

  if (slaveId.isSome()) {
    update.mutable_slave_id()->MergeFrom(slaveId.get());
  }

  if (executorId.isSome()) {
    update.mutable_executor_id()->MergeFrom(executorId.get());
  }

  TaskStatus* status = update.mutable_status();
  status->mutable_task_id()->MergeFrom(taskId);

  if (slaveId.isSome()) {
    status->mutable_slave_id()->MergeFrom(slaveId.get());
  }

  status->set_state(state);
  status->set_source(source);
  status->set_message(message);
  status->set_timestamp(update.timestamp());

  if (uuid.isSome()) {
    update.set_uuid(uuid.get().toBytes());
    status->set_uuid(uuid.get().toBytes());
  } else {
    // Note that in 0.22.x, the StatusUpdate.uuid was required
    // even though the scheduler driver ignores it for master
    // and scheduler driver generated updates. So we continue
    // to "set" it here so that updates coming from a 0.23.x
    // master can be parsed by a 0.22.x scheduler driver.
    //
    // TODO(bmahler): In 0.24.x, leave the uuid unset.
    update.set_uuid("");
  }

  if (reason.isSome()) {
    status->set_reason(reason.get());
  }

  if (healthy.isSome()) {
    status->set_healthy(healthy.get());
  }

  if (labels.isSome()) {
    status->mutable_labels()->CopyFrom(labels.get());
  }

  return update;
}


Task createTask(
    const TaskInfo& task,
    const TaskState& state,
    const FrameworkID& frameworkId)
{
  Task t;
  t.mutable_framework_id()->MergeFrom(frameworkId);
  t.set_state(state);
  t.set_name(task.name());
  t.mutable_task_id()->MergeFrom(task.task_id());
  t.mutable_slave_id()->MergeFrom(task.slave_id());
  t.mutable_resources()->MergeFrom(task.resources());

  if (task.has_executor()) {
    t.mutable_executor_id()->CopyFrom(task.executor().executor_id());
  }

  t.mutable_labels()->MergeFrom(task.labels());

  if (task.has_discovery()) {
    t.mutable_discovery()->MergeFrom(task.discovery());
  }

  return t;
}


Option<bool> getTaskHealth(const Task& task)
{
  Option<bool> healthy = None();
  if (task.statuses_size() > 0) {
    // The statuses list only keeps the most recent TaskStatus for
    // each state, and appends later states at the end. Thus the last
    // status is either a terminal state (where health is
    // irrelevant), or the latest RUNNING status.
    TaskStatus lastStatus = task.statuses(task.statuses_size() - 1);
    if (lastStatus.has_healthy()) {
      healthy = lastStatus.healthy();
    }
  }
  return healthy;
}

/**
 * Creates a MasterInfo protobuf from the process's UPID.
 *
 * This is only used by the `StandaloneMasterDetector` (used in tests
 * and outside tests when ZK is not used).
 *
 * For example, when we start a slave with
 * `--master=master@127.0.0.1:5050`, since the slave (and consequently
 * its detector) doesn't have enough information about `MasterInfo`, it
 * tries to construct it based on the only available information
 * (`UPID`).
 *
 * @param pid The process's assigned untyped PID.
 * @return A fully formed `MasterInfo` with the IP/hostname information
 *    as derived from the `UPID`.
 */
MasterInfo createMasterInfo(const process::UPID& pid)
{
  MasterInfo info;
  info.set_id(stringify(pid) + "-" + UUID::random().toString());

  // NOTE: Currently, we store the ip in network order, which should
  // be fixed. See MESOS-1201 for more details.
  info.set_ip(pid.address.ip.in().get().s_addr);

  info.set_port(pid.address.port);
  info.set_pid(pid);

  Try<string> hostname = net::getHostname(pid.address.ip);
  if (hostname.isSome()) {
    info.set_hostname(hostname.get());
  }

  return info;
}

} // namespace protobuf {
} // namespace internal {
} // namespace mesos {
