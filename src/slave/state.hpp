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

#ifndef __SLAVE_STATE_HPP__
#define __SLAVE_STATE_HPP__

#include <unistd.h>

#include <vector>

#include <process/pid.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "common/type_utils.hpp"

#include "messages/messages.hpp"

#include "process/pid.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace state {

// Forward declarations.
struct SlaveState;
struct FrameworkState;
struct ExecutorState;
struct RunState;
struct TaskState;

// Each of the structs below (recursively) recover the checkpointed
// state. If the 'safe' flag is set, any errors encountered while
// recovering a state are considered fatal and hence the recovery is
// short-circuited and returns an error. There might be orphaned
// executors that need to be manually cleaned up. If 'safe' flag is
// not set, any errors encountered are considered  non-fatal and the
// recovery continues by recovering as much of the state as possible.

struct SlaveState
{
  static Try<SlaveState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      bool safe);

  SlaveID id;
  Option<SlaveInfo> info;
  hashmap<FrameworkID, FrameworkState> frameworks;
};


struct FrameworkState
{
  static Try<FrameworkState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      bool safe);

  FrameworkID id;
  Option<FrameworkInfo> info;
  Option<process::UPID> pid;
  hashmap<ExecutorID, ExecutorState> executors;
};


struct ExecutorState
{
  static Try<ExecutorState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      bool safe);

  ExecutorID id;
  Option<ExecutorInfo> info;
  Option<UUID> latest;
  hashmap<UUID, RunState> runs;
};


struct RunState
{
  static Try<RunState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const UUID& uuid,
      bool safe);

  Option<UUID> id;
  hashmap<TaskID, TaskState> tasks;
  Option<pid_t> forkedPid;
  Option<process::UPID> libprocessPid;
};


struct TaskState
{
  static Try<TaskState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const UUID& uuid,
      const TaskID& taskId,
      bool safe);

  TaskID id;
  Option<Task> info;
  std::vector<StatusUpdate> updates;
  hashset<UUID> acks;
};


// This function performs recovery from the state stored at 'rootDir'.
Result<SlaveState> recover(const std::string& rootDir, bool safe);


// Thin wrappers to checkpoint data to disk and perform the
// necessary error checking.

// Checkpoints a protobuf at the given path.
Try<Nothing> checkpoint(
    const std::string& path,
    const google::protobuf::Message& message);


// Checkpoints a string at the given path.
Try<Nothing> checkpoint(const std::string& path, const std::string& message);

} // namespace state {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_STATE_HPP__
