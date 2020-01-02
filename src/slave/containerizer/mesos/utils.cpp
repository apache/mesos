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
// process would exist) and look to see if any 2-nd level process
// has a different `mnt` namespace. Only one pair of processes matches
// this property - command executor and its task. One important detail
// is that we skip all 1st-level processes whose `mnt` namespace is not
// the same as the `mnt` namespace of the `init` process.
//
// If we found such a 2-nd level process, we return a reference
// to its `pid` as the `pid` for entering the `mnt` namespace of the
// container.  Otherwise, we return the `init` process's `pid`.
//
// TODO(klueska): Remove this function once we start launching command
// tasks with the default (a.k.a. "pod" executor).
Try<pid_t> getMountNamespaceTarget(pid_t parent)
{
  Result<ino_t> parentNamespace = ns::getns(parent, "mnt");
  if (parentNamespace.isError()) {
    return Error("Cannot get 'mnt' namespace for process"
                 " '" + stringify(parent) + "': " + parentNamespace.error());
  } else if (parentNamespace.isNone()) {
    return Error("Cannot get 'mnt' namespace for non-existing process"
                 " '" + stringify(parent) + "'");
  }

  Try<set<pid_t>> children = os::children(parent, false);
  if (children.isError()) {
    return Error("Cannot get children for process"
                 " '" + stringify(parent) + "': " + children.error());
  }

  pid_t candidate = parent;
  int numCandidates = 0;
  bool hasGrandchild = false;

  foreach (pid_t child, children.get()) {
    Result<ino_t> childNamespace = ns::getns(child, "mnt");
    if (childNamespace.isError()) {
      return Error("Cannot get 'mnt' namespace for child process"
                   " '" + stringify(child) + "': " + childNamespace.error());
    } else if (childNamespace.isNone()) {
      VLOG(1) << "Cannot get 'mnt' namespace for non-existing child process"
                 " '" + stringify(child) + "'";
      continue;
    }

    if (parentNamespace.get() != childNamespace.get()) {
      // We skip this child, because we know that it's not a command executor.
      continue;
    }

    // Search for a new mount namespace in 2nd-level children.
    Try<set<pid_t>> children2 = os::children(child, false);
    if (children2.isError()) {
      return Error("Cannot get 2nd-level children for process"
                   " '" + stringify(parent) + "' with child"
                   " '" + stringify(child) + "': " + children2.error());
    }

    foreach (pid_t child2, children2.get()) {
      Result<ino_t> child2Namespace = ns::getns(child2, "mnt");
      if (child2Namespace.isError()) {
        return Error("Cannot get 'mnt' namespace for 2nd-level child process"
                     " '" + stringify(child2) +
                     "': " + child2Namespace.error());
      } else if (child2Namespace.isNone()) {
        VLOG(1) << "Cannot get 'mnt' namespace for non-existing 2nd-level"
                   " child process '" + stringify(child2) + "'";
        continue;
      }

      hasGrandchild = true;

      if (parentNamespace.get() != child2Namespace.get()) {
        ++numCandidates;
        candidate = child2;
      }
    }
  }

  if (!hasGrandchild) {
    return Error("Cannot detect task process: no 2nd-level processes found");
  }

  if (numCandidates > 1) {
    return Error("Cannot detect task process: unexpected number of candidates"
                 " found: " + stringify(numCandidates));
  }

  return candidate;
}
#endif // __linux__


Try<int> calculateOOMScoreAdj(const Bytes& memRequest)
{
  // Get the total memory of this node.
  static Option<Bytes> totalMem;
  if (totalMem.isNone()) {
    Try<os::Memory> mem = os::memory();
    if (mem.isError()) {
      return Error(
          "Failed to auto-detect the size of main memory: " + mem.error());
    }

    totalMem = mem->total;
  }

  CHECK_SOME(totalMem);

  return 1000 - (1000 * memRequest.bytes()) / totalMem->bytes();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
