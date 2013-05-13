#ifndef __STOUT_OS_KILLTREE_HPP__
#define __STOUT_OS_KILLTREE_HPP__

#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>

#include <list>
#include <ostream>
#include <queue>
#include <set>
#include <sstream>
#include <string>

#include <stout/os.hpp>
#include <stout/stringify.hpp>

namespace os {

// Forward declarations from os.hpp.
inline Try<std::set<pid_t> > children(pid_t pid, bool recursive);
inline Try<std::set<pid_t> > pids(Option<pid_t> group, Option<pid_t> session);


// Sends a signal to a process tree rooted at the specified pid.
// If groups is true, this also sends the signal to all encountered
// process groups.
// If sessions is true, this also sends the signal to all encountered
// process sessions.
// It is advised to only set groups / sessions when the process tree
// is guaranteed to be isolated via a setsid call.
// Optionally, an output stream can be provided for debugging output.
inline Try<Nothing> killtree(
    pid_t pid,
    int signal,
    bool groups = false,
    bool sessions = false,
    std::ostream* os = NULL)
{
  std::ostringstream output;
  output << "Performing killtree operation on " << pid << std::endl;

  // TODO(bmahler): Inspect parent session / group to ensure this
  // doesn't kill up the tree?

  // First we collect and stop the full process tree via a
  // breadth-first-search.
  std::set<pid_t> visited;
  std::queue<pid_t> queue;
  queue.push(pid);
  visited.insert(pid);

  while (!queue.empty()) {
    pid_t pid = queue.front();
    queue.pop();

    // Stop the process to keep it from forking while we are killing
    // it since a forked child might get re-parented by init and
    // become impossible to find.
    if (kill(pid, SIGSTOP) == -1) {
      output << "Failed to stop " << pid << ": "
             << strerror(errno) << std::endl;
    } else {
      output << "Stopped " << pid << std::endl;
    }

    // TODO(bmahler): Create and use sets::union here.
    // Append all direct children to the queue.
    const Try<std::set<pid_t> >& children = os::children(pid, false);
    if (children.isSome()) {
      output << "  Children of " << pid << ": "
             << stringify(children.get()) << std::endl;
      foreach (pid_t child, children.get()) {
        if (visited.insert(child).second) {
          queue.push(child);
        }
      }
    }

    if (groups) {
      // Append all group members to the queue.
      const Try<std::set<pid_t> >& pids = os::pids(getpgid(pid), None());
      if (pids.isSome()) {
        output << "  Members of group " << getpgid(pid) << ": "
               << stringify(pids.get()) << std::endl;
        foreach (pid_t pid, pids.get()) {
          if (visited.insert(pid).second) {
            queue.push(pid);
          }
        }
      }
    }

    if (sessions) {
      // Append all session members to the queue.
      const Try<std::set<pid_t> >& pids = os::pids(None(), getsid(pid));
      if (pids.isSome()) {
        output << "  Members of session " << getsid(pid) << ": "
               << stringify(pids.get()) << std::endl;
        foreach (pid_t pid, pids.get()) {
          if (visited.insert(pid).second) {
            queue.push(pid);
          }
        }
      }
    }
  }

  // Now that all processes are stopped, we send the signal.
  foreach (pid_t pid, visited) {
    kill(pid, signal);
    output << "Signaled " << pid << std::endl;
  }

  // There is a concern that even though some process is stopped,
  // sending a signal to any of it's children may cause a SIGCLD to
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
  foreach (pid_t pid, visited) {
    kill(pid, SIGCONT);
  }

  if (os != NULL) {
    *os << output.str();
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_KILLTREE_HPP__
