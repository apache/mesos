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

#include <mesos/type_utils.hpp>

#include <process/collect.hpp>
#include <process/delay.hpp>
#include <process/process.hpp>
#include <process/reap.hpp>

#include <stout/unreachable.hpp>

#include <stout/os/killtree.hpp>

#ifdef __linux__
#include "linux/systemd.hpp"
#endif // __linux__

#include "mesos/resources.hpp"

#include "slave/containerizer/mesos/launcher.hpp"

using namespace process;

using std::list;
using std::map;
using std::string;
using std::vector;

using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {

Try<Launcher*> PosixLauncher::create(const Flags& flags)
{
  return new PosixLauncher();
}


Future<hashset<ContainerID>> PosixLauncher::recover(
    const list<ContainerState>& states)
{
  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();
    pid_t pid = state.pid();

    if (pids.containsValue(pid)) {
      // This should (almost) never occur. There is the possibility
      // that a new executor is launched with the same pid as one that
      // just exited (highly unlikely) and the slave dies after the
      // new executor is launched but before it hears about the
      // termination of the earlier executor (also unlikely).
      // Regardless, the launcher can't do anything sensible so this
      // is considered an error.
      return Failure("Detected duplicate pid " + stringify(pid) +
                     " for container " + stringify(containerId));
    }

    pids.put(containerId, pid);
  }

  return hashset<ContainerID>();
}


Try<pid_t> PosixLauncher::fork(
    const ContainerID& containerId,
    const string& path,
    const vector<string>& argv,
    const Subprocess::IO& in,
    const Subprocess::IO& out,
    const Subprocess::IO& err,
    const flags::FlagsBase* flags,
    const Option<map<string, string>>& environment,
    const Option<int>& enterNamespaces,
    const Option<int>& cloneNamespaces)
{
  if (enterNamespaces.isSome() && enterNamespaces.get() != 0) {
    return Error("Posix launcher does not support entering namespaces");
  }

  if (cloneNamespaces.isSome() && cloneNamespaces.get() != 0) {
    return Error("Posix launcher does not support cloning namespaces");
  }

  if (pids.contains(containerId)) {
    return Error("Process has already been forked for container " +
                 stringify(containerId));
  }

  // If we are on systemd, then extend the life of the child. Any
  // grandchildren's lives will also be extended.
  vector<process::Subprocess::ParentHook> parentHooks;

#ifdef __linux__
  if (systemd::enabled()) {
    parentHooks.emplace_back(Subprocess::ParentHook(
        &systemd::mesos::extendLifetime));
  }
#elif defined(__WINDOWS__)
  parentHooks.emplace_back(Subprocess::ParentHook::CREATE_JOB());
#endif // __linux__

  Try<Subprocess> child = subprocess(
      path,
      argv,
      in,
      out,
      err,
      flags,
      environment,
      None(),
      parentHooks,
      {Subprocess::ChildHook::SETSID()});

  if (child.isError()) {
    return Error("Failed to fork a child process: " + child.error());
  }

  LOG(INFO) << "Forked child with pid '" << child.get().pid()
            << "' for container '" << containerId << "'";

  // Store the pid (session id and process group id).
  pids.put(containerId, child.get().pid());

  return child.get().pid();
}


// Forward declaration.
Future<Nothing> _destroy(const Future<Option<int>>& future);


Future<Nothing> PosixLauncher::destroy(const ContainerID& containerId)
{
  LOG(INFO) << "Asked to destroy container " << containerId;

  if (!pids.contains(containerId)) {
    LOG(WARNING) << "Ignored destroy for unknown container " << containerId;
    return Nothing();
  }

  pid_t pid = pids.get(containerId).get();

  // Kill all processes in the session and process group.
  Try<list<os::ProcessTree>> trees = os::killtree(pid, SIGKILL, true, true);

  pids.erase(containerId);

  // The child process may not have been waited on yet so we'll delay
  // completing destroy until we're sure it has been reaped.
  return process::reap(pid)
    .then(lambda::bind(&_destroy, lambda::_1));
}


Future<Nothing> _destroy(const Future<Option<int>>& future)
{
  if (future.isReady()) {
    return Nothing();
  } else {
    return Failure("Failed to kill all processes: " +
                   (future.isFailed() ? future.failure() : "unknown error"));
  }
}


Future<ContainerStatus> PosixLauncher::status(const ContainerID& containerId)
{
  if (!pids.contains(containerId)) {
    return Failure("Container does not exist!");
  }

  ContainerStatus status;
  status.set_executor_pid(pids[containerId]);

  return status;
}


Try<Launcher*> WindowsLauncher::create(const Flags& flags)
{
  return new WindowsLauncher();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
