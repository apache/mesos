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

#ifndef __STOUT_OS_FREEBSD_HPP__
#define __STOUT_OS_FREEBSD_HPP__

// This file contains FreeBSD-only OS utilities.
#ifndef __FreeBSD__
#error "stout/os/freebsd.hpp is only available on FreeBSD systems."
#endif

#include <sys/sysctl.h>
#include <sys/types.h>
#ifdef __FreeBSD__
#include <stout/os/freebsd.hpp>
#endif
#include <sys/user.h>
#include <unistd.h>

namespace os {

inline Result<Process> process(pid_t pid)
{
  // KERN_PROC_PID fails for zombies, so we fetch the whole process table and
  // find our process manually.

  const Try<std::vector<kinfo_proc>> kinfos =
    os::sysctl(CTL_KERN, KERN_PROC, KERN_PROC_ALL).table();

  if (kinfos.isError()) {
    return Error("Failed to retrieve process table via sysctl: " +
                 kinfos.error());
  }

  foreach (const kinfo_proc& kinfo, kinfos.get()) {
    if (kinfo.ki_pid == pid) {
      int pagesize = getpagesize();
      return Process(kinfo.ki_pid,
                     kinfo.ki_ppid,
                     kinfo.ki_pgid,
                     kinfo.ki_sid,
                     kinfo.ki_rssize * pagesize,
                     kinfo.ki_rusage.ru_utime,
                     kinfo.ki_rusage.ru_stime,
                     kinfo.ki_comm,
                     kinfo.ki_stat == SZOMB);
    }
  }

  return None();
}

inline Try<std::set<pid_t>> pids()
{
  std::set<pid_t> result;

  const Try<std::vector<kinfo_proc>> kinfos =
    os::sysctl(CTL_KERN, KERN_PROC, KERN_PROC_ALL).table();

  foreach (const kinfo_proc& kinfo, kinfos.get()) {
    result.insert(kinfo.ki_pid);
  }

  return result;
}
} // namespace os {

#endif
