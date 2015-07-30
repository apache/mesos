/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_OS_POSIX_HPP__
#define __STOUT_OS_POSIX_HPP__

// This file contains Linux-only OS utilities.
#ifndef __linux__
#error "stout/os/linux.hpp is only available on Linux systems."
#endif

#include <sys/types.h> // For pid_t.

#include <list>
#include <queue>
#include <set>
#include <string>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/proc.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include <stout/os/process.hpp>

namespace os {

inline Result<Process> process(pid_t pid)
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

  const Result<proc::ProcessStatus> status = proc::status(pid);

  if (status.isError()) {
    return Error(status.error());
  }

  if (status.isNone()) {
    return None();
  }

  // There are known bugs with invalid utime / stime values coming
  // from /proc/<pid>/stat on some Linux systems.
  // See the following thread for details:
  // http://mail-archives.apache.org/mod_mbox/incubator-mesos-dev/
  // 201307.mbox/%3CCA+2n2er-Nemh0CsKLbHRkaHd=YCrNt17NLUPM2=TtEfsKOw4
  // Rg@mail.gmail.com%3E
  // These are similar reports:
  // http://lkml.indiana.edu/hypermail/linux/kernel/1207.1/01388.html
  // https://bugs.launchpad.net/ubuntu/+source/linux/+bug/1023214
  Try<Duration> utime = Duration::create(status.get().utime / (double) ticks);
  Try<Duration> stime = Duration::create(status.get().stime / (double) ticks);

  // The command line from 'status.get().comm' is only "arg0" from
  // "argv" (i.e., the canonical executable name). To get the entire
  // command line we grab '/proc/[pid]/cmdline'.
  Result<std::string> cmdline = proc::cmdline(pid);

  return Process(status.get().pid,
                 status.get().ppid,
                 status.get().pgrp,
                 status.get().session,
                 Bytes(status.get().rss * pageSize),
                 utime.isSome() ? utime.get() : Option<Duration>::none(),
                 stime.isSome() ? stime.get() : Option<Duration>::none(),
                 cmdline.isSome() ? cmdline.get() : status.get().comm,
                 status.get().state == 'Z');
}


inline Try<std::set<pid_t> > pids()
{
  return proc::pids();
}

} // namespace os {

#endif // __STOUT_OS_POSIX_HPP__
