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
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/proc.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include <stout/os/pagesize.hpp>
#include <stout/os/process.hpp>

namespace os {


// Helper for clone() which expects an int(void*).
static int childMain(void* _func)
{
  const lambda::function<int()>* func =
    static_cast<const lambda::function<int()>*> (_func);

  return (*func)();
}


// Helper that captures information about a stack to be used when
// invoking clone.
struct Stack
{
  size_t size;
  unsigned long long* address;
};


inline pid_t clone(
    const lambda::function<int()>& func,
    int flags,
    Option<Stack> stack = None())
{
  // Stack for the child.
  // - unsigned long long used for best alignment.
  // - 8 MiB appears to be the default for "ulimit -s" on OSX and Linux.
  //
  // NOTE: We need to allocate the stack dynamically. This is because
  // glibc's 'clone' will modify the stack passed to it, therefore the
  // stack must NOT be shared as multiple 'clone's can be invoked
  // simultaneously.
  bool cleanup = false;
  if (stack.isNone()) {
    stack = Stack();
    stack->size = 8 * 1024 * 1024;
    stack->address =
      new unsigned long long[stack->size/sizeof(unsigned long long)];
    cleanup = true;
  }

  // Compute the address of the stack given that it grows down.
  void* address = &stack->address[stack->size / sizeof(stack->address[0]) - 1];

  pid_t pid = ::clone(childMain, address, flags, (void*) &func);

  // Given we allocated the stack ourselves, there are two
  // circumstances where we need to delete the allocated stack to
  // avoid a memory leak:
  //
  // (1) Failed to clone.
  //
  // (2) CLONE_VM is not set implying ::clone will create a process
  //     which runs in its own copy of the memory space of the
  //     calling process. If CLONE_VM is set ::clone will create a
  //     thread which runs in the same memory space with the calling
  //     process, in which case we don't want to call delete!
  if (cleanup && (pid < 0 || !(flags & CLONE_VM))) {
    delete[] stack->address;
  }

  return pid;
}


inline Result<Process> process(pid_t pid)
{
  // Page size, used for memory accounting.
  static const size_t pageSize = os::pagesize();

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


inline Try<std::set<pid_t>> pids()
{
  return proc::pids();
}


// Returns the total size of main and free memory.
inline Try<Memory> memory()
{
  Memory memory;

  struct sysinfo info;
  if (sysinfo(&info) != 0) {
    return ErrnoError();
  }

# if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 3, 23)
  memory.total = Bytes(info.totalram * info.mem_unit);
  memory.free = Bytes(info.freeram * info.mem_unit);
  memory.totalSwap = Bytes(info.totalswap * info.mem_unit);
  memory.freeSwap = Bytes(info.freeswap * info.mem_unit);
# else
  memory.total = Bytes(info.totalram);
  memory.free = Bytes(info.freeram);
  memory.totalSwap = Bytes(info.totalswap);
  memory.freeSwap = Bytes(info.freeswap);
# endif

  return memory;
}

} // namespace os {

#endif // __STOUT_OS_POSIX_HPP__
