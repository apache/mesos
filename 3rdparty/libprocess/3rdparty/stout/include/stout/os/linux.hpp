#ifndef __STOUT_OS_LINUX_HPP__
#define __STOUT_OS_LINUX_HPP__

// This file contains Linux-only OS utilities.
#ifndef __linux__
#error "stout/os/linux.hpp is only available on Linux systems."
#endif

#include <sys/types.h> // For pid_t.

#include <list>
#include <queue>
#include <set>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/proc.hpp>
#include <stout/try.hpp>

#include <stout/os/process.hpp>

namespace os {

inline Try<Process> process(pid_t pid)
{
  // Page size, used for memory accounting.
  // NOTE: This is more portable than using getpagesize().
  static const long pageSize = sysconf(_SC_PAGESIZE);
  if (pageSize <= 0) {
    return Error("Failed to get sysconf(_SC_PAGESIZE)");
  }

  // Number of clock ticks per second, used for cpu accounting.
  static const long ticks = sysconf(_SC_CLK_TCK);
  if (ticks <= 0) {
    return Error("Failed to get sysconf(_SC_CLK_TCK)");
  }

  const Try<proc::ProcessStatus>& status = proc::status(pid);

  if (status.isError()) {
    return Error(status.error());
  }

  return Process(status.get().pid,
                 status.get().ppid,
                 status.get().pgrp,
                 status.get().session,
                 Bytes(status.get().rss * pageSize),
                 Duration::create(status.get().utime / (double) ticks).get(),
                 Duration::create(status.get().stime / (double) ticks).get(),
                 status.get().comm);
}


inline Try<std::set<pid_t> > pids()
{
  return proc::pids();
}

} // namespace os {

#endif // __STOUT_OS_LINUX_HPP__
