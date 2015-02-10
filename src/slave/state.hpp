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

#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <process/pid.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "messages/messages.hpp"

namespace mesos {
namespace slave {
namespace state {

// Forward declarations.
struct State;
struct SlaveState;
struct FrameworkState;
struct ExecutorState;
struct RunState;
struct TaskState;
struct ResourcesState;

// This function performs recovery from the state stored at 'rootDir'.
// If the 'strict' flag is set, any errors encountered while
// recovering a state are considered fatal and hence the recovery is
// short-circuited and returns an error. There might be orphaned
// executors that need to be manually cleaned up. If 'strict' flag is
// not set, any errors encountered are considered non-fatal and the
// recovery continues by recovering as much of the state as possible,
// while increasing the 'errors' count. Note that 'errors' on a struct
// includes the 'errors' encountered recursively. In other words,
// 'State.errors' is the sum total of all recovery errors. If the
// machine has rebooted since the last slave run, None is returned.
Result<State> recover(const std::string& rootDir, bool strict);

namespace internal {

inline Try<Nothing> checkpoint(
    const std::string& path,
    const std::string& message)
{
  return ::os::write(path, message);
}


inline Try<Nothing> checkpoint(
    const std::string& path,
    const google::protobuf::Message& message)
{
  return ::protobuf::write(path, message);
}


template <typename T>
Try<Nothing> checkpoint(
    const std::string& path,
    const google::protobuf::RepeatedPtrField<T>& messages)
{
  return ::protobuf::write(path, messages);
}


inline Try<Nothing> checkpoint(
    const std::string& path,
    const Resources& resources)
{
  const google::protobuf::RepeatedPtrField<Resource>& messages = resources;
  return checkpoint(path, messages);
}

} // namespace internal {


// Thin wrapper to checkpoint data to disk and perform the necessary
// error checking. It checkpoints an instance of T at the given path.
// We can checkpoint anything as long as T is supported by
// internal::checkpoint. Currently the list of supported Ts are:
//   - std::string
//   - google::protobuf::Message
//   - google::protobuf::RepeatedPtrField<T>
//   - mesos::Resources
//
// NOTE: We provide atomic (all-or-nothing) semantics here by always
// writing to a temporary file first then use os::rename to atomically
// move it to the desired path.
template <typename T>
Try<Nothing> checkpoint(const std::string& path, const T& t)
{
  // Create the base directory.
  Try<std::string> base = os::dirname(path);
  if (base.isError()) {
    return Error("Failed to get the base directory path: " + base.error());
  }

  Try<Nothing> mkdir = os::mkdir(base.get());
  if (mkdir.isError()) {
    return Error("Failed to create directory '" + base.get() +
                 "': " + mkdir.error());
  }

  // NOTE: We create the temporary file at 'base/XXXXXX' to make sure
  // rename below does not cross devices (MESOS-2319).
  //
  // TODO(jieyu): It's possible that the temporary file becomes
  // dangling if slave crashes or restarts while checkpointing.
  // Consider adding a way to garbage collect them.
  Try<std::string> temp = os::mktemp(path::join(base.get(), "XXXXXX"));
  if (temp.isError()) {
    return Error("Failed to create temporary file: " + temp.error());
  }

  // Now checkpoint the instance of T to the temporary file.
  Try<Nothing> checkpoint = internal::checkpoint(temp.get(), t);
  if (checkpoint.isError()) {
    // Try removing the temporary file on error.
    os::rm(temp.get());

    return Error("Failed to write temporary file '" + temp.get() +
                 "': " + checkpoint.error());
  }

  // Rename the temporary file to the path.
  Try<Nothing> rename = os::rename(temp.get(), path);
  if (rename.isError()) {
    // Try removing the temporary file on error.
    os::rm(temp.get());

    return Error("Failed to rename '" + temp.get() + "' to '" +
                 path + "': " + rename.error());
  }

  return Nothing();
}


// The top level state. Each of the structs below (recursively)
// recover the checkpointed state.
struct State
{
  State() : errors(0) {}

  Option<ResourcesState> resources;
  Option<SlaveState> slave;

  // TODO(jieyu): Consider using a vector of Option<Error> here so
  // that we can print all the errors. This also applies to all the
  // State structs below.
  unsigned int errors;
};


struct ResourcesState
{
  ResourcesState() : errors(0) {}

  static Try<ResourcesState> recover(
      const std::string& rootDir,
      bool strict);

  Resources resources;
  unsigned int errors;
};


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

  // NOTE: We create the executor directory before checkpointing the
  // executor. Therefore, it's not possible for this directory to be
  // non-existent.
  std::string directory;

  // Executor terminated and all its updates acknowledged.
  bool completed;

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
} // namespace mesos {

#endif // __SLAVE_STATE_HPP__
