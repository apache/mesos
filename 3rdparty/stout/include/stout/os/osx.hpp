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

#ifndef __STOUT_OS_OSX_HPP__
#define __STOUT_OS_OSX_HPP__

// This file contains OSX-only OS utilities.
#ifndef __APPLE__
#error "stout/os/osx.hpp is only available on OSX systems."
#endif

#include <libproc.h>

#include <sys/sysctl.h>
#include <sys/types.h> // For pid_t.

#include <queue>
#include <set>
#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/strings.hpp>

#include <stout/os/os.hpp>
#include <stout/os/pagesize.hpp>
#include <stout/os/process.hpp>
#include <stout/os/sysctl.hpp>

namespace os {

inline Result<Process> process(pid_t pid)
{
  const Try<std::vector<kinfo_proc>> processes =
    os::sysctl(CTL_KERN, KERN_PROC, KERN_PROC_PID, pid).table(1);

  if (processes.isError()) {
    return Error("Failed to get process via sysctl: " + processes.error());
  } else if (processes->size() != 1) {
    return None();
  }

  const kinfo_proc process = processes.get()[0];

  // The command line from 'process.kp_proc.p_comm' only includes the
  // first 16 characters from "arg0" (i.e., the canonical executable
  // name). We can try to get "argv" via some sysctl magic. This first
  // requires determining "argc" via KERN_PROCARGS2 followed by the
  // actual arguments via KERN_PROCARGS. This is still insufficient
  // with insufficient privilege (e.g., not being root). If we were
  // only interested in the "executable path" (i.e., the first
  // argument to 'exec' but none of the arguments) we could use
  // proc_pidpath() instead.
  Option<std::string> command = None();

  // Alternative implemenations using KERN_PROCARGS2:
  // https://bitbucket.org/flub/psi-win32/src/d38f288de3b8/src/arch/macosx/macosx_process.c#cl-552
  // https://gist.github.com/nonowarn/770696

#ifdef KERN_PROCARGS2
  // Looking at the source code of XNU (the Darwin kernel for OS X:
  // www.opensource.apple.com/source/xnu/xnu-1699.24.23/bsd/kern/kern_sysctl.c),
  // it appears as though KERN_PROCARGS2 writes 'argc' as the first
  // word of the returned bytes.
  Try<std::string> args = os::sysctl(CTL_KERN, KERN_PROCARGS2, pid).string();

  if (args.isSome()) {
    int argc = *((int*) args->data());

    if (argc > 0) {
      // Now grab the arguments.
      args = os::sysctl(CTL_KERN, KERN_PROCARGS, pid).string();

      if (args.isSome()) {
        // At this point 'args' contains the parameters to 'exec'
        // delimited by null bytes, i.e., "executable path", then
        // "arg0" (the canonical executable name), then "arg1", then
        // "arg2", etc. Sometimes there are no arguments (argc = 1) so
        // all we care about is the "executable path", but when there
        // are arguments we grab "arg0" and on assuming that "arg0"
        // really is the canonical executable name.

        // Tokenize the args by the null byte ('\0').
        std::vector<std::string> tokens =
          strings::tokenize(args.get(), std::string(1, '\0'));

        if (!tokens.empty()) {
          if (argc == 1) {
            // When there are no arguments, all we care about is the
            // "executable path".
            command = tokens[0];
          } else if (argc > 1) {
            // When there are arguments, we skip the "executable path"
            // and just grab "arg0" -> "argN", assuming "arg0" is the
            // canonical executable name. In the case that we didn't
            // get enough tokens back from KERN_PROCARGS the following
            // code will end up just keeping 'command' None (i.e.,
            // tokens.size() will be <= 0).
            tokens.erase(tokens.begin()); // Remove path.

            // In practice it appeared that we might get an 'argc'
            // which is greater than 'tokens.size()' which caused us
            // to get an invalid iterator from 'tokens.begin() +
            // argc', thus we now check that condition here.
            if (static_cast<size_t>(argc) <= tokens.size()) {
              tokens.erase(tokens.begin() + argc, tokens.end());
            }

            // If any tokens are left, consider them the command!
            if (tokens.size() > 0) {
              command = strings::join(" ", tokens);
            }
          }
        }
      }
    }
  }
#endif

  // We also use proc_pidinfo() to get memory and CPU usage.
  // NOTE: There are several pitfalls to using proc_pidinfo().
  // In particular:
  //   -This will not work for many root processes.
  //   -This may not work for processes owned by other users.
  //   -However, this always works for processes owned by the same user.
  // This beats using task_for_pid(), which only works for the same pid.
  // For further discussion around these issues,
  // see: http://code.google.com/p/psutil/issues/detail?id=297
  proc_taskinfo task;
  int size = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof(task));

  // It appears that zombie processes on OS X do not have sessions and
  // result in ESRCH.
  int session = getsid(pid);

  if (size != sizeof(task)) {
    return Process(process.kp_proc.p_pid,
                   process.kp_eproc.e_ppid,
                   process.kp_eproc.e_pgid,
                   session > 0 ? session : Option<pid_t>::none(),
                   None(),
                   None(),
                   None(),
                   command.getOrElse(std::string(process.kp_proc.p_comm)),
                   process.kp_proc.p_stat & SZOMB);
  } else {
    return Process(process.kp_proc.p_pid,
                   process.kp_eproc.e_ppid,
                   process.kp_eproc.e_pgid,
                   session > 0 ? session : Option<pid_t>::none(),
                   Bytes(task.pti_resident_size),
                   Nanoseconds(task.pti_total_user),
                   Nanoseconds(task.pti_total_system),
                   command.getOrElse(std::string(process.kp_proc.p_comm)),
                   process.kp_proc.p_stat & SZOMB);
  }
}


inline Try<std::set<pid_t>> pids()
{
  const Try<int> maxproc = os::sysctl(CTL_KERN, KERN_MAXPROC).integer();

  if (maxproc.isError()) {
    return Error(maxproc.error());
  }

  const Try<std::vector<kinfo_proc>> processes =
    os::sysctl(CTL_KERN, KERN_PROC, KERN_PROC_ALL).table(maxproc.get());

  if (processes.isError()) {
    return Error(processes.error());
  }

  std::set<pid_t> result;
  foreach (const kinfo_proc& process, processes.get()) {
    result.insert(process.kp_proc.p_pid);
  }
  return result;
}


// Returns the total size of main and free memory.
inline Try<Memory> memory()
{
  Memory memory;

  const Try<int64_t> totalMemory = os::sysctl(CTL_HW, HW_MEMSIZE).integer();

  if (totalMemory.isError()) {
    return Error(totalMemory.error());
  }
  memory.total = Bytes(totalMemory.get());

  // Size of free memory is available in terms of number of
  // free pages on Mac OS X.
  const size_t pageSize = os::pagesize();

  unsigned int freeCount;
  size_t length = sizeof(freeCount);

  if (sysctlbyname(
      "vm.page_free_count",
      &freeCount,
      &length,
      nullptr,
      0) != 0) {
    return ErrnoError();
  }
  memory.free = Bytes(freeCount * pageSize);

  struct xsw_usage usage;
  length = sizeof(struct xsw_usage);
  if (sysctlbyname(
        "vm.swapusage",
        &usage,
        &length,
        nullptr,
        0) != 0) {
    return ErrnoError();
  }
  memory.totalSwap = Bytes(usage.xsu_total * pageSize);
  memory.freeSwap = Bytes(usage.xsu_avail * pageSize);

  return memory;
}

} // namespace os {

#endif // __STOUT_OS_OSX_HPP__
