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

// This function performs recovery from the state stored at 'rootDir'.
// If the 'strict' flag is set, any errors encountered while
// recovering a state are considered fatal and hence the recovery is
// short-circuited and returns an error. There might be orphaned
// executors that need to be manually cleaned up. If 'strict' flag is
// not set, any errors encountered are considered non-fatal and the
// recovery continues by recovering as much of the state as possible,
// while increasing the 'errors' count. Note that 'errors' on a struct
// includes the 'errors' encountered recursively. In other words,
// 'SlaveState.errors' is the sum total of all recovery errors.
// If the machine has rebooted since the last slave run,
// None is returned.
Result<SlaveState> recover(const std::string& rootDir, bool strict);

// Thin wrappers to checkpoint data to disk and perform the
// necessary error checking.

// Checkpoints a protobuf at the given path.
Try<Nothing> checkpoint(
    const std::string& path,
    const google::protobuf::Message& message);


// Checkpoints a string at the given path.
Try<Nothing> checkpoint(const std::string& path, const std::string& message);

// Each of the structs below (recursively) recover the checkpointed
// state.
struct SlaveState
{
  SlaveState () : errors(0) {}

  static Try<SlaveState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      bool strict);

  SlaveID id;
  Option<SlaveInfo> info;
  hashmap<FrameworkID, FrameworkState> frameworks;
  unsigned int errors;
};


struct FrameworkState
{
  FrameworkState () : errors(0) {}

  static Try<FrameworkState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      bool strict);

  FrameworkID id;
  Option<FrameworkInfo> info;
  Option<process::UPID> pid;
  hashmap<ExecutorID, ExecutorState> executors;
  unsigned int errors;
};


struct ExecutorState
{
  ExecutorState () : errors(0) {}

  static Try<ExecutorState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      bool strict);

  ExecutorID id;
  Option<ExecutorInfo> info;
  Option<ContainerID> latest;
  hashmap<ContainerID, RunState> runs;
  unsigned int errors;
};


struct RunState
{
  RunState () : completed(false), errors(0) {}

  static Try<RunState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ContainerID& containerId,
      bool strict);

  Option<ContainerID> id;
  hashmap<TaskID, TaskState> tasks;
  Option<pid_t> forkedPid;
  Option<process::UPID> libprocessPid;
  bool completed; // Executor terminated and all its updates acknowledged.
  unsigned int errors;
};


struct TaskState
{
  TaskState () : errors(0) {}

  static Try<TaskState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ContainerID& containerId,
      const TaskID& taskId,
      bool strict);

  TaskID id;
  Option<Task> info;
  std::vector<StatusUpdate> updates;
  hashset<UUID> acks;
  unsigned int errors;
};

} // namespace state {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_STATE_HPP__
