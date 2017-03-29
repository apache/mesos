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

#ifndef __STOUT_OS_POSIX_KILLTREE_HPP__
#define __STOUT_OS_POSIX_KILLTREE_HPP__

#include <stdlib.h>
#include <unistd.h>

#include <list>
#include <queue>
#include <set>

#include <stout/check.hpp>
#include <stout/os.hpp>

#include <stout/os/pstree.hpp>


namespace os {

// Forward declarations from os.hpp.
inline std::set<pid_t> children(pid_t, const std::list<Process>&, bool);
inline Result<Process> process(pid_t);
inline Option<Process> process(pid_t, const std::list<Process>&);
inline Try<std::list<Process>> processes();
inline Try<std::list<ProcessTree>> pstrees(
    const std::set<pid_t>&,
    const std::list<Process>&);


// Sends a signal to a process tree rooted at the specified pid.
// If groups is true, this also sends the signal to all encountered
// process groups.
// If sessions is true, this also sends the signal to all encountered
// process sessions.
// Note that processes of the group and session of the parent of the
// root process is not included unless they are part of the root
// process tree.
// Note that if the process 'pid' has exited we'll signal the process
// tree(s) rooted at pids in the group or session led by the process
// if groups = true or sessions = true, respectively.
// Returns the process trees that were successfully or unsuccessfully
// signaled. Note that the process trees can be stringified.
// TODO(benh): Allow excluding the root pid from stopping, killing,
// and continuing so as to provide a means for expressing "kill all of
// my children". This is non-trivial because of the current
// implementation.
inline Try<std::list<ProcessTree>> killtree(
    pid_t pid,
    int signal,
    bool groups = false,
    bool sessions = false)
{
  Try<std::list<Process>> processes = os::processes();

  if (processes.isError()) {
    return Error(processes.error());
  }

  Result<Process> process = os::process(pid, processes.get());

  std::queue<pid_t> queue;

  // If the root process has already terminated we'll add in any pids
  // that are in the process group originally led by pid or in the
  // session originally led by pid, if instructed.
  if (process.isNone()) {
    foreach (const Process& _process, processes.get()) {
      if (groups && _process.group == pid) {
        queue.push(_process.pid);
      } else if (sessions &&
                 _process.session.isSome() &&
                 _process.session.get() == pid) {
        queue.push(_process.pid);
      }
    }

    // Root process is not running and no processes found in the
    // process group or session so nothing we can do.
    if (queue.empty()) {
      return std::list<ProcessTree>();
    }
  } else {
    // Start the traversal from pid as the root.
    queue.push(pid);
  }

  struct {
    std::set<pid_t> pids;
    std::set<pid_t> groups;
    std::set<pid_t> sessions;
    std::list<Process> processes;
  } visited;

  // If we are following groups and/or sessions then we try and make
  // the group and session of the parent process "already visited" so
  // that we don't kill "up the tree". This can only be done if the
  // process is present.
  if (process.isSome() && (groups || sessions)) {
    Option<Process> parent =
      os::process(process.get().parent, processes.get());

    if (parent.isSome()) {
      if (groups) {
        visited.groups.insert(parent.get().group);
      }
      if (sessions && parent.get().session.isSome()) {
        visited.sessions.insert(parent.get().session.get());
      }
    }
  }

  while (!queue.empty()) {
    pid_t pid = queue.front();
    queue.pop();

    if (visited.pids.count(pid) != 0) {
      continue;
    }

    // Make sure this process still exists.
    process = os::process(pid);

    if (process.isError()) {
      return Error(process.error());
    } else if (process.isNone()) {
      continue;
    }

    // Stop the process to keep it from forking while we are killing
    // it since a forked child might get re-parented by init and
    // become impossible to find.
    kill(pid, SIGSTOP);

    visited.pids.insert(pid);
    visited.processes.push_back(process.get());

    // Now refresh the process list knowing that the current process
    // can't fork any more children (since it's stopped).
    processes = os::processes();

    if (processes.isError()) {
      return Error(processes.error());
    }

    // Enqueue the children for visiting.
    foreach (pid_t child, os::children(pid, processes.get(), false)) {
      queue.push(child);
    }

    // Now "visit" the group and/or session of the current process.
    if (groups) {
      pid_t group = process.get().group;
      if (visited.groups.count(group) == 0) {
        foreach (const Process& process, processes.get()) {
          if (process.group == group) {
            queue.push(process.pid);
          }
        }
        visited.groups.insert(group);
      }
    }

    // If we do not have a session for the process, it's likely
    // because the process is a zombie on OS X. This implies it has
    // not been reaped and thus is located somewhere in the tree we
    // are trying to kill. Therefore, we should discover it from our
    // tree traversal, or through its group (which is always present).
    if (sessions && process.get().session.isSome()) {
      pid_t session = process.get().session.get();
      if (visited.sessions.count(session) == 0) {
        foreach (const Process& process, processes.get()) {
          if (process.session.isSome() && process.session.get() == session) {
            queue.push(process.pid);
          }
        }
        visited.sessions.insert(session);
      }
    }
  }

  // Now that all processes are stopped, we send the signal.
  foreach (pid_t pid, visited.pids) {
    kill(pid, signal);
  }

  // There is a concern that even though some process is stopped,
  // sending a signal to any of its children may cause a SIGCLD to
  // be delivered to it which wakes it up (or any other signal maybe
  // delivered). However, from the Open Group standards on "Signal
  // Concepts":
  //
  //   "While a process is stopped, any additional signals that are
  //    sent to the process shall not be delivered until the process
  //    is continued, except SIGKILL which always terminates the
  //    receiving process."
  //
  // In practice, this is not what has been witnessed. Rather, a
  // process that has been stopped will respond to SIGTERM, SIGINT,
  // etc. That being said, we still continue the process below in the
  // event that it doesn't terminate from the sending signal but it
  // also doesn't get continued (as per the specifications above).

  // Try and continue the processes in case the signal is
  // non-terminating but doesn't continue the process.
  foreach (pid_t pid, visited.pids) {
    kill(pid, SIGCONT);
  }

  // Return the process trees representing the visited pids.
  return pstrees(visited.pids, visited.processes);
}

} // namespace os {

#endif // __STOUT_OS_POSIX_KILLTREE_HPP__
