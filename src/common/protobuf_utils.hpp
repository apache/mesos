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

#ifndef __PROTOBUF_UTILS_HPP__
#define __PROTOBUF_UTILS_HPP__

#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/uuid.hpp>

#include "common/type_utils.hpp"

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace protobuf {

inline bool isTerminalState(const TaskState& state)
{
  return (state == TASK_FINISHED ||
          state == TASK_FAILED ||
          state == TASK_KILLED ||
          state == TASK_LOST);
}


inline StatusUpdate createStatusUpdate(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const TaskID& taskId,
    const TaskState& state,
    const std::string& message,
    const ExecutorID& executorId = ExecutorID())
{
  StatusUpdate update;

  update.mutable_framework_id()->MergeFrom(frameworkId);

  if (!(executorId == "")) {
    update.mutable_executor_id()->MergeFrom(executorId);
  }

  update.mutable_slave_id()->MergeFrom(slaveId);
  TaskStatus* status = update.mutable_status();
  status->mutable_task_id()->MergeFrom(taskId);
  status->set_state(state);
  status->set_message(message);

  update.set_timestamp(::process::Clock::now());
  update.set_uuid(UUID::random().toBytes());

  return update;
}


inline Task createTask(const TaskInfo& task,
                       const TaskState& state,
                       const ExecutorID& executorId,
                       const FrameworkID& frameworkId)
{
  Task t;
  t.mutable_framework_id()->MergeFrom(frameworkId);
  t.set_state(state);
  t.set_name(task.name());
  t.mutable_task_id()->MergeFrom(task.task_id());
  t.mutable_slave_id()->MergeFrom(task.slave_id());
  t.mutable_resources()->MergeFrom(task.resources());

  if (!task.has_command()) {
    t.mutable_executor_id()->MergeFrom(executorId);
  }

  return t;
}

} // namespace protobuf
} // namespace internal {
} // namespace mesos {

#endif // __PROTOBUF_UTILS_HPP__
