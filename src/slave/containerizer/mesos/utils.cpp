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

#include <set>

#include <stout/os.hpp>
#include <stout/try.hpp>

#ifdef __linux__
#include "linux/ns.hpp"
#endif // __linux__

#include "slave/containerizer/mesos/utils.hpp"

using std::set;

namespace mesos {
namespace internal {
namespace slave {

#ifdef __linux__
// This function can be used to find a new target `pid` for entering
// the `mnt` namespace of a container (if necessary).
//
// Until we switch over to the default (a.k.a. "pod" executor) for
// launching command tasks, we need to special case which `pid` we
// use for entering the `mnt` namespace of a parent container.
// Specifically, we need to enter the `mnt` namespace of the
// process representing the command task itself, not the `mnt`
// namespace of the `init` process of the container or the
// `executor` of the container because these run in the same `mnt`
// namespace as the agent (not the task).
//
// Unfortunately, there is no easy way to get the `pid` of tasks
// launched with the command executor because we only checkpoint the
// `pid` of the `init` process of these containers. For now, we
// compensate for this by simply walking the process tree from the
// container's `init` process up to 2-levels down (where the task
// process would exist) and look to see if any process along the way
// has a different `mnt` namespace. If it does, we return a reference
// to its `pid` as the `pid` for entering the `mnt` namespace of the
// container.  Otherwise, we return the `init` process's `pid`.
//
// TODO(klueska): Remove this function once we start launching command
// tasks with the default (a.k.a. "pod" executor).
Try<pid_t> getMountNamespaceTarget(pid_t parent)
{
  Try<ino_t> parentNamespace = ns::getns(parent, "mnt");
  if (parentNamespace.isError()) {
    return Error("Cannot get 'mnt' namespace for"
                 " process '" + stringify(parent) + "'");
  }

  // Search for a new mount namespace in all direct children.
  Try<set<pid_t>> children = os::children(parent, false);
  if (children.isError()) {
    return Error("Cannot get children for process"
                 " '" + stringify(parent) + "'");
  }

  foreach (pid_t child, children.get()) {
    Try<ino_t> childNamespace = ns::getns(child, "mnt");
    if (childNamespace.isError()) {
      return Error("Cannot get 'mnt' namespace for"
                   " child process '" + stringify(child) + "'");
    }

    if (parentNamespace.get() != childNamespace.get()) {
      return child;
    }
  }

  // Search for a new mount namespace in all 2nd-level children.
  foreach (pid_t child, children.get()) {
    Try<set<pid_t>> children2 = os::children(child, false);
    if (children2.isError()) {
      return Error("Cannot get 2nd-level children for process"
                   " '" + stringify(parent) + "' with child"
                   " '" + stringify(child) + "'");
    }

    foreach (pid_t child2, children2.get()) {
      Try<ino_t> child2Namespace = ns::getns(child2, "mnt");
      if (child2Namespace.isError()) {
        return Error("Cannot get 'mnt' namespace for 2nd-level"
                     " child process '" + stringify(child2) + "'");
      }

      if (parentNamespace.get() != child2Namespace.get()) {
        return child2;
      }
    }
  }

  return parent;
}
#endif // __linux__

} // namespace slave {
} // namespace internal {
} // namespace mesos {
