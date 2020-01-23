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

#ifndef __SLAVE_STATE_HPP__
#define __SLAVE_STATE_HPP__

#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#include <vector>

#include <mesos/resources.hpp>
#include <mesos/type_utils.hpp>

#include <process/pid.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include <stout/os/mkdir.hpp>
#include <stout/os/mktemp.hpp>
#include <stout/os/rename.hpp>
#include <stout/os/rm.hpp>
#include <stout/os/write.hpp>

#include "common/resources_utils.hpp"

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace state {

// Forward declarations.
struct State;
struct SlaveState;
struct ResourcesState;
struct FrameworkState;
struct ExecutorState;
struct RunState;
struct TaskState;


// This function performs recovery from the state stored at 'rootDir'.
// If the 'strict' flag is set, any errors encountered while
// recovering a state are considered fatal and hence the recovery is
// short-circuited and returns an error. There might be orphaned
// executors that need to be manually cleaned up. If the 'strict' flag
// is not set, any errors encountered are considered non-fatal and the
// recovery continues by recovering as much of the state as possible,
// while increasing the 'errors' count. Note that 'errors' on a struct
// includes the 'errors' encountered recursively. In other words,
// 'State.errors' is the sum total of all recovery errors.
Try<State> recover(const std::string& rootDir, bool strict);


// Reads the protobuf message(s) from the given path.
// `T` may be either a single protobuf message or a sequence of messages
// if `T` is a specialization of `google::protobuf::RepeatedPtrField`.
template <typename T>
Result<T> read(const std::string& path)
{
  Result<T> result = ::protobuf::read<T>(path);
  if (result.isSome()) {
    upgradeResources(&result.get());
  }

  return result;
}


// While we return a `Result<string>` here in order to keep the return
// type of `state::read` consistent, the `None` case does not arise here.
// That is, an empty file will result in an empty string, rather than
// the `Result` ending up in a `None` state.
template <>
inline Result<std::string> read<std::string>(const std::string& path)
{
  return os::read(path);
}


template <>
inline Result<Resources> read<Resources>(const std::string& path)
{
  Result<google::protobuf::RepeatedPtrField<Resource>> resources =
    read<google::protobuf::RepeatedPtrField<Resource>>(path);

  if (resources.isError()) {
    return Error(resources.error());
  }

  if (resources.isNone()) {
    return None();
  }

  return std::move(resources.get());
}


namespace internal {

inline Try<Nothing> checkpoint(
    const std::string& path,
    const std::string& message,
    bool sync,
    bool downgradeResources)
{
  return ::os::write(path, message, sync);
}


template <
    typename T,
    typename std::enable_if<
        std::is_convertible<T*, google::protobuf::Message*>::value,
        int>::type = 0>
inline Try<Nothing> checkpoint(
    const std::string& path,
    T message,
    bool sync,
    bool downgrade)
{
  if (downgrade) {
    // If the `Try` from `downgradeResources` returns an `Error`, we currently
    // continue to checkpoint the resources in a partially downgraded state.
    // This implies that an agent with refined reservations cannot be downgraded
    // to versions before reservation refinement support, which was introduced
    // in 1.4.0.
    //
    // TODO(mpark): Do something smarter with the result once
    // something like an agent recovery capability is introduced.
    downgradeResources(&message);
  }

  return ::protobuf::write(path, message, sync);
}


inline Try<Nothing> checkpoint(
    const std::string& path,
    google::protobuf::RepeatedPtrField<Resource> resources,
    bool sync,
    bool downgrade)
{
  if (downgrade) {
    // If the `Try` from `downgradeResources` returns an `Error`, we currently
    // continue to checkpoint the resources in a partially downgraded state.
    // This implies that an agent with refined reservations cannot be downgraded
    // to versions before reservation refinement support, which was introduced
    // in 1.4.0.
    //
    // TODO(mpark): Do something smarter with the result once
    // something like an agent recovery capability is introduced.
    downgradeResources(&resources);
  }

  return ::protobuf::write(path, resources, sync);
}


inline Try<Nothing> checkpoint(
    const std::string& path,
    const Resources& resources,
    bool sync,
    bool downgrade)
{
  const google::protobuf::RepeatedPtrField<Resource>& messages = resources;
  return checkpoint(path, messages, sync, downgrade);
}

}  // namespace internal {


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
// writing to a temporary file first then using os::rename to atomically
// move it to the desired path. If `sync` is set to true, this call succeeds
// only if `fsync` is supported and successfully commits the changes to the
// filesystem for the checkpoint file and each created directory.
//
// TODO(chhsiao): Consider enabling syncing by default after evaluating its
// performance impact.
template <typename T>
Try<Nothing> checkpoint(
    const std::string& path,
    const T& t,
    bool sync = false,
    bool downgrade = true)
{
  // Create the base directory.
  std::string base = Path(path).dirname();

  Try<Nothing> mkdir = os::mkdir(base, true, sync);
  if (mkdir.isError()) {
    return Error("Failed to create directory '" + base + "': " + mkdir.error());
  }

  // NOTE: We create the temporary file at 'base/XXXXXX' to make sure
  // rename below does not cross devices (MESOS-2319).
  //
  // TODO(jieyu): It's possible that the temporary file becomes
  // dangling if slave crashes or restarts while checkpointing.
  // Consider adding a way to garbage collect them.
  Try<std::string> temp = os::mktemp(path::join(base, "XXXXXX"));
  if (temp.isError()) {
    return Error("Failed to create temporary file: " + temp.error());
  }

  // Now checkpoint the instance of T to the temporary file.
  Try<Nothing> checkpoint =
    internal::checkpoint(temp.get(), t, sync, downgrade);
  if (checkpoint.isError()) {
    // Try removing the temporary file on error.
    os::rm(temp.get());

    return Error("Failed to write temporary file '" + temp.get() +
                 "': " + checkpoint.error());
  }

  // Rename the temporary file to the path.
  Try<Nothing> rename = os::rename(temp.get(), path, sync);
  if (rename.isError()) {
    // Try removing the temporary file on error.
    os::rm(temp.get());

    return Error("Failed to rename '" + temp.get() + "' to '" +
                 path + "': " + rename.error());
  }

  return Nothing();
}


// NOTE: The *State structs (e.g., TaskState, RunState, etc) are
// defined in reverse dependency order because many of them have
// Option<*State> dependencies which means we need them declared in
// their entirety in order to compile because things like
// Option<*State> need to know the final size of the types.

struct TaskState
{
  TaskState() : errors(0) {}

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
  hashset<id::UUID> acks;
  unsigned int errors;
};


struct RunState
{
  RunState() : completed(false), errors(0) {}

  static Try<RunState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const ContainerID& containerId,
      bool strict,
      bool rebooted);

  Option<ContainerID> id;
  hashmap<TaskID, TaskState> tasks;
  Option<pid_t> forkedPid;
  Option<process::UPID> libprocessPid;

  // This represents if the executor is connected via HTTP. It can be None()
  // when the connection type is unknown.
  Option<bool> http;

  // Executor terminated and all its updates acknowledged.
  bool completed;

  unsigned int errors;
};


struct ExecutorState
{
  ExecutorState() : errors(0) {}

  static Try<ExecutorState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      bool strict,
      bool rebooted);

  ExecutorID id;
  Option<ExecutorInfo> info;
  Option<ContainerID> latest;
  hashmap<ContainerID, RunState> runs;
  unsigned int errors;
  bool generatedForCommandTask = false;
};


struct FrameworkState
{
  FrameworkState() : errors(0) {}

  static Try<FrameworkState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      bool strict,
      bool rebooted);

  FrameworkID id;
  Option<FrameworkInfo> info;

  // Note that HTTP frameworks (supported in 0.24.0) do not have a
  // PID, in which case 'pid' is Some(UPID()) rather than None().
  Option<process::UPID> pid;

  hashmap<ExecutorID, ExecutorState> executors;
  unsigned int errors;
};


struct ResourcesState
{
  ResourcesState() : errors(0) {}

  static Try<ResourcesState> recover(
      const std::string& rootDir,
      bool strict);

  Resources resources;
  Option<Resources> target;
  unsigned int errors;
};


struct SlaveState
{
  SlaveState() : errors(0) {}

  static Try<SlaveState> recover(
      const std::string& rootDir,
      const SlaveID& slaveId,
      bool strict,
      bool rebooted);

  SlaveID id;
  Option<SlaveInfo> info;
  hashmap<FrameworkID, FrameworkState> frameworks;

  // `operations` will be `None()` if the agent that checkpointed the
  // state didn't support checkpointing operations.
  Option<std::vector<Operation>> operations;

  // The drain state of the agent, if any.
  Option<DrainConfig> drainConfig;

  unsigned int errors;
};


// The top level state. The members are child nodes in the tree. Each
// child node (recursively) recovers the checkpointed state.
struct State
{
  State() : errors(0) {}

  Option<ResourcesState> resources;
  Option<SlaveState> slave;
  bool rebooted = false;

  // TODO(jieyu): Consider using a vector of Option<Error> here so
  // that we can print all the errors. This also applies to all the
  // State structs above.
  unsigned int errors;
};

} // namespace state {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_STATE_HPP__
