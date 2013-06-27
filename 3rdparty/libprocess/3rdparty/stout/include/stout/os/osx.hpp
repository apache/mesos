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

#include <stout/os/process.hpp>
#include <stout/os/sysctl.hpp>

namespace os {

inline Result<Process> process(pid_t pid)
{
  const Try<std::vector<kinfo_proc> >& processes =
    os::sysctl(CTL_KERN, KERN_PROC, KERN_PROC_PID, pid).table(1);

  if (processes.isError()) {
    return Error("Failed to get process via sysctl: " + processes.error());
  } else if (processes.get().size() != 1) {
    return None();
  }

  // We use proc_pidinfo() as it provides memory and CPU usage.
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

  const kinfo_proc process = processes.get()[0];

  if (size != sizeof(task)) {
    return Process(process.kp_proc.p_pid,
                   process.kp_eproc.e_ppid,
                   process.kp_eproc.e_pgid,
                   getsid(pid),
                   Bytes(0),
                   Nanoseconds(-1),
                   Nanoseconds(-1),
                   process.kp_proc.p_comm,
                   process.kp_proc.p_stat & SZOMB);
  } else {
    return Process(process.kp_proc.p_pid,
                   process.kp_eproc.e_ppid,
                   process.kp_eproc.e_pgid,
                   getsid(pid),
                   Bytes(task.pti_resident_size),
                   Nanoseconds(task.pti_total_user),
                   Nanoseconds(task.pti_total_system),
                   process.kp_proc.p_comm,
                   process.kp_proc.p_stat & SZOMB);
  }
}


inline Try<std::set<pid_t> > pids()
{
  const Try<int>& maxproc = os::sysctl(CTL_KERN, KERN_MAXPROC).integer();

  if (maxproc.isError()) {
    return Error(maxproc.error());
  }

  const Try<std::vector<kinfo_proc> >& processes =
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

} // namespace os {

#endif // __STOUT_OS_OSX_HPP__
