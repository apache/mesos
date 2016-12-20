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

#ifndef __STOUT_OS_SUNOS_HPP__
#define __STOUT_OS_SUNOS_HPP__

// This file contains Solaris-only OS utilities.
#ifndef __sun
#error "stout/os/sunos.hpp is only available on Solaris systems."
#endif

#include <fcntl.h>
#include <procfs.h>
#include <sys/types.h> // For pid_t.

#include <list>
#include <set>
#include <string>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include <stout/os/int_fd.hpp>
#include <stout/os/open.hpp>
#include <stout/os/process.hpp>

namespace os {

inline Result<Process> process(pid_t pid)
{
  std::string fn = "/proc/" + stringify(pid) + "/status";
  struct pstatus pstatus;

  Try<int_fd> fd = os::open(fn, O_RDONLY);
  if (fd.isError()) {
    return ErrnoError("Cannot open " + fn);
  }

  if (::read(fd.get(), &pstatus, sizeof(pstatus)) != sizeof(pstatus)) {
    os::close(fd.get());
    return Error("Cannot read from " + fn);
  }
  os::close(fd.get());

  fn = "/proc/" + stringify(pid) + "/psinfo";
  struct psinfo psinfo;

  fd = os::open(fn, O_RDONLY);
  if (fd.isError()) {
    return ErrnoError("Cannot open " + fn);
  }

  if (::read(fd.get(), &psinfo, sizeof(psinfo)) != sizeof(psinfo)) {
    os::close(fd.get());
    return Error("Cannot read from " + fn);
  }
  os::close(fd.get());

  Try<Duration> utime =
    Seconds(pstatus.pr_utime.tv_sec) + Nanoseconds(pstatus.pr_utime.tv_nsec);
  Try<Duration> stime =
    Seconds(pstatus.pr_stime.tv_sec) + Nanoseconds(pstatus.pr_stime.tv_nsec);

  return Process(pstatus.pr_pid,
                 pstatus.pr_ppid,
                 pstatus.pr_ppid,
                 pstatus.pr_sid,
                 None(),
                 utime.isSome() ? utime.get() : Option<Duration>::none(),
                 stime.isSome() ? stime.get() : Option<Duration>::none(),
                 psinfo.pr_fname,
                 (psinfo.pr_nzomb == 0) &&
                  (psinfo.pr_nlwp == 0) &&
                  (psinfo.pr_lwp.pr_lwpid == 0));
}

// Reads from /proc and returns a list of all running processes.
inline Try<std::set<pid_t>> pids()
{
  std::set<pid_t> pids;

  Try<std::list<std::string>> entries = os::ls("/proc");
  if (entries.isError()) {
    return Error("Failed to list files in /proc: " + entries.error());
  }

  foreach (const std::string& entry, entries.get()) {
    Try<pid_t> pid = numify<pid_t>(entry);
    if (pid.isSome()) {
      pids.insert(pid.get()); // Ignore entries that can't be numified.
    }
  }

  if (!pids.empty()) {
    return pids;
  }

  return Error("Failed to determine pids from /proc");
}


// Returns the total size of main and free memory.
inline Try<Memory> memory()
{
  return Error("Cannot determine the size of total and free memory");
}

} // namespace os {

#endif // __STOUT_OS_SUNOS_HPP__
