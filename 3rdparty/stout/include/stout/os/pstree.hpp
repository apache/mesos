// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_OS_PSTREE_HPP__
#define __STOUT_OS_PSTREE_HPP__

#include <list>
#include <set>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/os/process.hpp>


namespace os {

// Forward declaration.
inline Try<std::list<Process>> processes();


// Returns a process tree rooted at the specified pid using the
// specified list of processes (or an error if one occurs).
inline Try<ProcessTree> pstree(
    pid_t pid,
    const std::list<Process>& processes)
{
  std::list<ProcessTree> children;
  foreach (const Process& process, processes) {
    if (process.parent == pid) {
      Try<ProcessTree> tree = pstree(process.pid, processes);
      if (tree.isError()) {
        return Error(tree.error());
      }
      children.push_back(tree.get());
    }
  }

  foreach (const Process& process, processes) {
    if (process.pid == pid) {
      return ProcessTree(process, children);
    }
  }

  return Error("No process found at " + stringify(pid));
}


// Returns a process tree for the specified pid (or all processes if
// pid is none or the current process if pid is 0).
inline Try<ProcessTree> pstree(Option<pid_t> pid = None())
{
  if (pid.isNone()) {
    pid = 1;
  } else if (pid.get() == 0) {
    pid = getpid();
  }

  const Try<std::list<Process>> processes = os::processes();

  if (processes.isError()) {
    return Error(processes.error());
  }

  return pstree(pid.get(), processes.get());
}


// Returns the minimum list of process trees that include all of the
// specified pids using the specified list of processes.
inline Try<std::list<ProcessTree>> pstrees(
    const std::set<pid_t>& pids,
    const std::list<Process>& processes)
{
  std::list<ProcessTree> trees;

  foreach (pid_t pid, pids) {
    // First, check if the pid is already connected to one of the
    // process trees we've constructed.
    bool disconnected = true;
    foreach (const ProcessTree& tree, trees) {
      if (tree.contains(pid)) {
        disconnected = false;
        break;
      }
    }

    if (disconnected) {
      Try<ProcessTree> tree = pstree(pid, processes);
      if (tree.isError()) {
        return Error(tree.error());
      }

      // Now see if any of the existing process trees are actually
      // contained within the process tree we just created and only
      // include the disjoint process trees.
      // C++11:
      // trees = trees.filter([](const ProcessTree& t) {
      //   return tree.get().contains(t);
      // });
      std::list<ProcessTree> trees_ = trees;
      trees.clear();
      foreach (const ProcessTree& t, trees_) {
        if (tree->contains(t.process.pid)) {
          continue;
        }
        trees.push_back(t);
      }
      trees.push_back(tree.get());
    }
  }

  return trees;
}

} // namespace os {

#endif // __STOUT_OS_PSTREE_HPP__
