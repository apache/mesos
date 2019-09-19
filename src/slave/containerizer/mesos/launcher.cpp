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
#ifdef __WINDOWS__
#include <process/windows/jobobject.hpp>
#endif // __WINDOWS__

#include <stout/unreachable.hpp>

#include <stout/os/killtree.hpp>

#ifdef __linux__
#include "linux/systemd.hpp"
#endif // __linux__

#include "mesos/resources.hpp"

#include "slave/containerizer/mesos/launcher.hpp"

using namespace process;

using std::map;
using std::string;
using std::vector;

using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {

Try<Launcher*> SubprocessLauncher::create(const Flags& flags)
{
  return new SubprocessLauncher();
}


Future<hashset<ContainerID>> SubprocessLauncher::recover(
    const vector<ContainerState>& states)
{
  foreach (const ContainerState& state, states) {
    const ContainerID& containerId = state.container_id();
    pid_t pid = static_cast<pid_t>(state.pid());

    if (pids.contains_value(pid)) {
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


Try<pid_t> SubprocessLauncher::fork(
    const ContainerID& containerId,
    const string& path,
    const vector<string>& argv,
    const mesos::slave::ContainerIO& containerIO,
    const flags::FlagsBase* flags,
    const Option<map<string, string>>& environment,
    const Option<int>& enterNamespaces,
    const Option<int>& cloneNamespaces,
    const vector<int_fd>& whitelistFds)
{
  if (enterNamespaces.isSome() && enterNamespaces.get() != 0) {
    return Error("Subprocess launcher does not support entering namespaces");
  }

  if (cloneNamespaces.isSome() && cloneNamespaces.get() != 0) {
    return Error("Subprocess launcher does not support cloning namespaces");
  }

  if (pids.contains(containerId)) {
    return Error("Process has already been forked for container " +
                 stringify(containerId));
  }

  vector<process::Subprocess::ParentHook> parentHooks;

#ifdef __linux__
  // If we are on systemd, then extend the life of the child. Any
  // grandchildren's lives will also be extended.
  if (systemd::enabled()) {
    parentHooks.emplace_back(Subprocess::ParentHook(
        &systemd::mesos::extendLifetime));
  }
#elif defined(__WINDOWS__)
  // If we are on Windows, then ensure the child is placed inside a
  // new job object.
  parentHooks.emplace_back(Subprocess::ParentHook::CREATE_JOB());
#endif // __linux__

  vector<Subprocess::ChildHook> childHooks;

#ifndef __WINDOWS__
  childHooks.push_back(Subprocess::ChildHook::SETSID());
#endif // __WINDOWS__

  Try<Subprocess> child = subprocess(
      path,
      argv,
      containerIO.in,
      containerIO.out,
      containerIO.err,
      flags,
      environment,
      None(),
      parentHooks,
      childHooks,
      whitelistFds);

  if (child.isError()) {
    return Error("Failed to fork a child process: " + child.error());
  }

  LOG(INFO) << "Forked child with pid '" << child->pid()
            << "' for container '" << containerId << "'";

  // Store the pid (session id and process group id).
  pids.put(containerId, child->pid());

  return child->pid();
}


// Forward declaration.
Future<Nothing> _destroy(const Future<Option<int>>& future);


Future<Nothing> SubprocessLauncher::destroy(const ContainerID& containerId)
{
  LOG(INFO) << "Asked to destroy container " << containerId;

  if (!pids.contains(containerId)) {
    LOG(WARNING) << "Ignored destroy for unknown container " << containerId;
    return Nothing();
  }

  pid_t pid = pids.at(containerId);

  // Kill all processes in the session and process group.
  os::killtree(pid, SIGKILL, true, true);

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


Future<ContainerStatus> SubprocessLauncher::status(
    const ContainerID& containerId)
{
  if (!pids.contains(containerId)) {
    return Failure("Container does not exist!");
  }

  ContainerStatus status;
  status.set_executor_pid(pids[containerId]);

  return status;
}


} // namespace slave {
} // namespace internal {
} // namespace mesos {
