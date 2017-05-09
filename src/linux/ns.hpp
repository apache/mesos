// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __LINUX_NS_HPP__
#define __LINUX_NS_HPP__

// This file contains Linux-only OS utilities.
#ifndef __linux__
#error "linux/ns.hpp is only available on Linux systems."
#endif

#include <assert.h>
#include <sched.h>
#include <unistd.h>

#include <sys/syscall.h>
#include <sys/wait.h>

#include <set>
#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/proc.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/ls.hpp>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/reap.hpp>

#include "common/status_utils.hpp"

#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif

#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS 0x04000000
#endif

#ifndef CLONE_NEWIPC
#define CLONE_NEWIPC 0x08000000
#endif

#ifndef CLONE_NEWPID
#define CLONE_NEWPID 0x20000000
#endif

#ifndef CLONE_NEWNET
#define CLONE_NEWNET 0x40000000
#endif

#ifndef CLONE_NEWUSER
#define CLONE_NEWUSER 0x10000000
#endif

#ifndef CLONE_NEWCGROUP
#define CLONE_NEWCGROUP 0x02000000
#endif

// Define a 'setns' for compilation environments that don't already
// have one.
inline int setns(int fd, int nstype)
{
#ifdef SYS_setns
  return ::syscall(SYS_setns, fd, nstype);
#elif defined(__x86_64__)
  // A workaround for those hosts that have an old glibc (older than
  // 2.14) but have a new kernel. The magic number '308' here is the
  // syscall number for 'setns' on x86_64 architecture.
  return ::syscall(308, fd, nstype);
#else
#error "setns is not available"
#endif
}

namespace ns {

// Returns the nstype (e.g., CLONE_NEWNET, CLONE_NEWNS, etc.) for the
// given namespace which can be used when calling ::setns.
inline Try<int> nstype(const std::string& ns)
{
  const hashmap<std::string, int> nstypes = {
    {"mnt", CLONE_NEWNS},
    {"uts", CLONE_NEWUTS},
    {"ipc", CLONE_NEWIPC},
    {"net", CLONE_NEWNET},
    {"user", CLONE_NEWUSER},
    {"pid", CLONE_NEWPID},
    {"cgroup", CLONE_NEWCGROUP}
  };

  Option<int> nstype = nstypes.get(ns);

  if (nstype.isNone()) {
    return Error("Unknown namespace '" + ns + "'");
  }

  return nstype.get();
}


// Returns all the supported namespaces by the kernel.
inline std::set<std::string> namespaces()
{
  std::set<std::string> result;
  Try<std::list<std::string>> entries = os::ls("/proc/self/ns");
  if (entries.isSome()) {
    foreach (const std::string& entry, entries.get()) {
      result.insert(entry);
    }
  }
  return result;
}


// Returns all the supported namespaces by the kernel.
inline std::set<int> nstypes()
{
  std::set<int> result;
  foreach (const std::string& ns, namespaces()) {
    Try<int> type = nstype(ns);
    if (type.isSome()) {
      result.insert(type.get());
    }
  }
  return result;
}


// Re-associate the calling process with the specified namespace. The
// path refers to one of the corresponding namespace entries in the
// /proc/[pid]/ns/ directory (or bind mounted elsewhere). We do not
// allow a process with multiple threads to call this function because
// it will lead to some weird situations where different threads of a
// process are in different namespaces.
inline Try<Nothing> setns(
    const std::string& path,
    const std::string& ns,
    bool checkMultithreaded = true)
{
  if (checkMultithreaded) {
    // Return error if there're multiple threads in the calling process.
    Try<std::set<pid_t>> threads = proc::threads(::getpid());
    if (threads.isError()) {
      return Error(
          "Failed to get the threads of the current process: " +
          threads.error());
    } else if (threads.get().size() > 1) {
      return Error("Multiple threads exist in the current process");
    }
  }

  if (ns::namespaces().count(ns) == 0) {
    return Error("Namespace '" + ns + "' is not supported");
  }

  // Currently, we don't support pid namespace as its semantics is
  // different from other namespaces (instead of re-associating the
  // calling thread, it re-associates the *children* of the calling
  // thread with the specified namespace).
  if (ns == "pid") {
    return Error("Pid namespace is not supported");
  }

  Try<int> fd = os::open(path, O_RDONLY | O_CLOEXEC);

  if (fd.isError()) {
    return Error("Failed to open '" + path + "': " + fd.error());
  }

  Try<int> nstype = ns::nstype(ns);
  if (nstype.isError()) {
    return Error(nstype.error());
  }

  if (::setns(fd.get(), nstype.get()) == -1) {
    // Save the errno as it might be overwritten by 'os::close' below.
    ErrnoError error;
    os::close(fd.get());
    return error;
  }

  os::close(fd.get());
  return Nothing();
}


// Re-associate the calling process with the specified namespace. The
// pid specifies the process whose namespace we will associate.
inline Try<Nothing> setns(pid_t pid, const std::string& ns)
{
  if (!os::exists(pid)) {
    return Error("Pid " + stringify(pid) + " does not exist");
  }

  std::string path = path::join("/proc", stringify(pid), "ns", ns);
  if (!os::exists(path)) {
    return Error("Namespace '" + ns + "' is not supported");
  }

  return ns::setns(path, ns);
}


// Get the inode number of the specified namespace for the specified
// pid. The inode number identifies the namespace and can be used for
// comparisons, i.e., two processes with the same inode for a given
// namespace type are in the same namespace.
inline Try<ino_t> getns(pid_t pid, const std::string& ns)
{
  if (!os::exists(pid)) {
    return Error("Pid " + stringify(pid) + " does not exist");
  }

  if (ns::namespaces().count(ns) < 1) {
    return Error("Namespace '" + ns + "' is not supported");
  }

  std::string path = path::join("/proc", stringify(pid), "ns", ns);
  struct stat s;
  if (::stat(path.c_str(), &s) < 0) {
    return ErrnoError("Failed to stat " + ns + " namespace handle"
                      " for pid " + stringify(pid));
  }

  return s.st_ino;
}



/**
 * Performs an `os::clone` after entering a set of namespaces for the
 * specified `target` process.
 *
 * This function provides two steps of functionality:
 *   (1) Enter a set of namespaces via two `fork` calls.
 *   (1) Perform a `clone` within that set of namespaces.
 *
 * Step (1) of functionality is similar to the `nsenter` command line
 * utility. Step (2) allows us to perform a clone that itself might
 * create a nested set of namespaces, which enables us to have nested
 * containers.
 *
 * Double Fork:
 *
 * In order to enter a PID namespace we need to do a double fork
 * because doing a `setns` for a PID namespace only effects future
 * children.
 *
 * Moreover, attempting to `setns` before we do any forks and then
 * have the parent `setns` back to the original namespaces does not
 * work because entering a depriviledged user namespace will not let
 * us reassociate back with the original namespace, even if we keep
 * the file descriptor of the original namespace open.
 *
 * Because we have to double fork we need to send back the actual PID
 * of the final process that's executing the provided function `f`.
 * We use domain sockets for this because in the event we've entered a
 * PID namespace we need the kernel to translate the PID to the PID in
 * our PID namespace.
 *
 * @param target Target process whose namespaces we should enter.
 * @param nstypes Namespaces we should enter.
 * @param f Function to invoke after entering the namespaces and cloning.
 * @param flags Flags to pass to `clone`.
 *
 * @return `pid_t` of the child process.
 */
inline Try<pid_t> clone(
    pid_t target,
    int nstypes,
    const lambda::function<int()>& f,
    int flags)
{
  // NOTE: the order in which we 'setns' is significant, so we use an
  // array here rather than something like a map.
  //
  // The user namespace needs to be entered first if we need to
  // increase the privilege and last if we want to decrease the
  // privilege. Said another way, entering the user namespace first
  // gives an unprivileged user the potential to enter the other
  // namespaces.
  const size_t NAMESPACES = 7;
  struct
  {
    int nstype;
    std::string name;
  } namespaces[NAMESPACES] = {
    {CLONE_NEWUSER, "user"},
    {CLONE_NEWCGROUP, "cgroup"},
    {CLONE_NEWIPC, "ipc"},
    {CLONE_NEWUTS, "uts"},
    {CLONE_NEWNET, "net"},
    {CLONE_NEWPID, "pid"},
    {CLONE_NEWNS, "mnt"}
  };

  // Support for user namespaces in all filesystems is incomplete
  // until version 3.12 (see 'Availability' in man page of
  // 'user_namespaces'), so for now we don't support entering them.
  //
  // TODO(benh): Support user namespaces if the current system can
  // support it, e.g., check the kernel version number or try and do a
  // clone with CLONE_NEWUSER to see if it works. NOTE: before we can
  // fully support user namespaces, however, we must take care to
  // either enter the user namespace first or last. We'll want to
  // enter it first if we need to increase the privilege and last if
  // we want to decrease the privilege. Currently nsenter.c from
  // utils-linux does this via doing two passes to make sure we either
  // enter first or last. We'll need to do something similar here once
  // we support user namespaces as well.
  if (nstypes & CLONE_NEWUSER) {
    return Error("User namespaces are not supported");
  }

  // File descriptors keyed by the (parent) namespace we are entering.
  hashmap<int, int> fds = {};

  // Helper for closing a list of file descriptors.
  auto close = [](const std::list<int>& fds) {
    foreach (int fd, fds) {
      ::close(fd); // Need to call the async-signal safe version.
    }
  };

  // NOTE: we do all of this ahead of time so we can be async signal
  // safe after calling fork below.
  for (size_t i = 0; i < NAMESPACES; i++) {
    // Only open the namespace file descriptor if it's been requested.
    if (namespaces[i].nstype & nstypes) {
      std::string path =
        path::join("/proc", stringify(target), "ns", namespaces[i].name);
      Try<int> fd = os::open(path, O_RDONLY);
      if (fd.isError()) {
        close(fds.values());
        return Error("Failed to open '" + path +
                     "' for entering namespace: " + fd.error());
      }
      fds[namespaces[i].nstype] = fd.get();
    }
  }

  // We use a domain socket rather than pipes so that we can send back
  // the PID of the final child process. The parent socket is
  // `sockets[0]` and the child socket is `sockets[1]`. Note that both
  // sockets are both read/write but currently only the parent reads
  // and the child writes.
  int sockets[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    close(fds.values());
    return ErrnoError("Failed to create Unix domain socket");
  }

  // Need to set SO_PASSCRED option in order to receive credentials
  // (which is how we get the pid of the clone'd process, see
  // below). Note that apparently we only need to do this for
  // receiving, not also for sending.
  const int value = 1;
  const ssize_t size = sizeof(value);
  if (setsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &value, size) == -1) {
    Error error = ErrnoError("Failed to set socket option SO_PASSCRED");
    close(fds.values());
    ::close(sockets[0]);
    ::close(sockets[1]);
    return error;
  }

  // NOTE: to determine the pid of the final process executing the
  // specified lambda we use the SCM_CREDENTIALS mechanism of
  // 'sendmsg' and 'recvmsg'. On Linux there is also a way to do this
  // via 'getsockopt' and SO_PEERCRED which looks easier, but IIUC
  // requires you to do an explicit connect from the child process
  // back to the parent so that there is only one connection per
  // socket (unlike in our world where the socket can be used by
  // multiple forks/clones simultaneously because it's just a file
  // descriptor that gets copied after each fork/clone). Perhaps the
  // SO_PEERCRED is less lines of code but this approach was taken for
  // now.

  char base[1];

  iovec iov = {0};
  iov.iov_base = base;
  iov.iov_len = sizeof(base);

  // Need to allocate a char array large enough to hold "control"
  // data. However, since this buffer is in reality a 'struct cmsghdr'
  // we use a union to ensure that it is aligned as required for that
  // structure.
  union {
    struct cmsghdr cmessage;
    char control[CMSG_SPACE(sizeof(struct ucred))];
  };

  cmessage.cmsg_len = CMSG_LEN(sizeof(struct ucred));
  cmessage.cmsg_level = SOL_SOCKET;
  cmessage.cmsg_type = SCM_CREDENTIALS;

  msghdr message = {0};
  message.msg_name = nullptr;
  message.msg_namelen = 0;
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control); // CMSG_LEN(sizeof(struct ucred));

  // Finally, the stack we'll use in the call to os::clone below (we
  // allocate the stack here in order to keep the call to os::clone
  // async signal safe, since otherwise it would be doing the dynamic
  // allocation itself).
  Try<os::Stack> stack = os::Stack::create(os::Stack::DEFAULT_SIZE);
  if (stack.isError()) {
    return Error("Failed to allocate stack: " + stack.error());
  }

  pid_t child = fork();
  if (child < 0) {
    stack->deallocate();
    close(fds.values());
    ::close(sockets[0]);
    ::close(sockets[1]);
    return ErrnoError();
  } else if (child > 0) {
    // Parent.
    stack->deallocate();

    close(fds.values());
    ::close(sockets[1]);

    ssize_t length = recvmsg(sockets[0], &message, 0);

    // TODO(benh): Note that whenever we 'kill(child, SIGKILL)' below
    // we don't guarantee cleanup! It's possible that the
    // greatgrandchild is still running. Require the greatgrandchild
    // to read from the socket after sending back it's pid to ensure
    // no orphans.

    if (length < 0) {
      // We failed to read, close the socket and kill the child
      // (which might die on it's own trying to write to the
      // socket).
      Error error = ErrnoError("Failed to receive");
      ::close(sockets[0]);
      kill(child, SIGKILL);
      return error;
    } else if (length == 0) {
      // Socket closed, child must have died, but kill anyway.
      ::close(sockets[0]);
      kill(child, SIGKILL);
      return Error("Failed to receive: Socket closed");
    }

    ::close(sockets[0]);

    // Extract pid.
    if (CMSG_FIRSTHDR(&message) == nullptr ||
        CMSG_FIRSTHDR(&message)->cmsg_len != CMSG_LEN(sizeof(struct ucred)) ||
        CMSG_FIRSTHDR(&message)->cmsg_level != SOL_SOCKET ||
        CMSG_FIRSTHDR(&message)->cmsg_type != SCM_CREDENTIALS) {
      kill(child, SIGKILL);
      return Error("Bad control data received");
    }

    pid_t pid = ((struct ucred*) CMSG_DATA(CMSG_FIRSTHDR(&message)))->pid;

    // Need to `waitpid` on child process to avoid a zombie. Note that
    // it's expected that the child will terminate quickly hence
    // blocking here.
    int status;
    while (true) {
      if (waitpid(child, &status, 0) == -1) {
        if (errno == EINTR) {
          continue;
        } else {
          return ErrnoError("Failed to `waitpid` on child");
        }
      } else if (WIFSTOPPED(status)) {
        continue;
      } else {
        break;
      }
    }

    CHECK(WIFEXITED(status) || WIFSIGNALED(status))
      << "Unexpected wait status " << status;

    if (!WSUCCEEDED(status)) {
      return Error("Failed to clone: " + WSTRINGIFY(status));
    }

    return pid;
  } else {
    // Child.
    ::close(sockets[0]);

    // Loop through and 'setns' into all of the parent namespaces that
    // have been requested.
    for (size_t i = 0; i < NAMESPACES; i++) {
      Option<int> fd = fds.get(namespaces[i].nstype);
      if (fd.isSome()) {
        assert(namespaces[i].nstype & nstypes);
        if (::setns(fd.get(), namespaces[i].nstype) < 0) {
          close(fds.values());
          ::close(sockets[1]);
          _exit(EXIT_FAILURE);
        }
      }
    }

    close(fds.values());

    // Fork again to make sure we're actually in those namespaces
    // (required for the pid namespace at least).
    //
    // TODO(benh): Don't do a fork if we're not actually entering the
    // PID namespace since the extra fork is unnecessary.
    pid_t grandchild = fork();
    if (grandchild < 0) {
      // TODO(benh): Exit with `errno` in order to capture `fork` error?
      ::close(sockets[1]);
      _exit(EXIT_FAILURE);
    } else if (grandchild > 0) {
      // Still the (first) child.
      ::close(sockets[1]);

      // Need to reap the grandchild and then just exit since we're no
      // longer necessary. Technically when the grandchild exits it'll
      // be reaped but by doing a `waitpid` we can better propagate
      // back any errors that might have occurred with the grandchild.
      int status;
      while (true) {
        if (waitpid(grandchild, &status, 0) == -1) {
          if (errno == EINTR) {
            continue;
          } else {
            _exit(1);
          }
        } else if (WIFSTOPPED(status)) {
          continue;
        } else {
          break;
        }
      }

      assert(WIFEXITED(status) || WIFSIGNALED(status));

      if (WIFEXITED(status)) {
        _exit(WEXITSTATUS(status));
      }

      assert(WIFSIGNALED(status));
      raise(WTERMSIG(status));
    }

    // Grandchild (second child, now completely entered in the
    // namespaces of the target).
    //
    // Now clone with the specified flags, close the unused socket,
    // and execute the specified function.
    pid_t pid = os::clone([=]() {
      // Now send back the pid and have it be translated appropriately
      // by the kernel to the enclosing pid namespace.
      //
      // NOTE: sending back the pid is best effort because we're going
      // to exit no matter what.
      ((struct ucred*) CMSG_DATA(CMSG_FIRSTHDR(&message)))->pid = ::getpid();
      ((struct ucred*) CMSG_DATA(CMSG_FIRSTHDR(&message)))->uid = ::getuid();
      ((struct ucred*) CMSG_DATA(CMSG_FIRSTHDR(&message)))->gid = ::getgid();

      if (sendmsg(sockets[1], &message, 0) == -1) {
        // Failed to send the pid back to the parent!
        _exit(EXIT_FAILURE);
      }

      ::close(sockets[1]);

      return f();
    },
    flags,
    stack.get());

    ::close(sockets[1]);

    // TODO(benh): Kill ourselves with an exit status that we can
    // decode above to determine why `clone` failed.
    _exit(pid < 0 ? EXIT_FAILURE : EXIT_SUCCESS);
  }
  UNREACHABLE();
}


namespace pid {

inline process::Future<Nothing> destroy(ino_t inode)
{
  // Check we're not trying to kill the root namespace.
  Try<ino_t> ns = ns::getns(1, "pid");
  if (ns.isError()) {
    return process::Failure(ns.error());
  }

  if (ns.get() == inode) {
    return process::Failure("Cannot destroy root pid namespace");
  }

  // Or ourselves.
  ns = ns::getns(::getpid(), "pid");
  if (ns.isError()) {
    return process::Failure(ns.error());
  }

  if (ns.get() == inode) {
    return process::Failure("Cannot destroy own pid namespace");
  }

  // Signal all pids in the namespace, including the init pid if it's
  // still running. Once the init pid has been signalled the kernel
  // will prevent any new children forking in the namespace and will
  // also signal all other pids in the namespace.
  Try<std::set<pid_t>> pids = os::pids();
  if (pids.isError()) {
    return process::Failure("Failed to list of processes");
  }

  foreach (pid_t pid, pids.get()) {
    // Ignore any errors, probably because the process no longer
    // exists, and ignorable otherwise.
    Try<ino_t> ns = ns::getns(pid, "pid");
    if (ns.isSome() && ns.get() == inode) {
      kill(pid, SIGKILL);
    }
  }

  // Get a new snapshot and do a second pass of the pids to capture
  // any pids that are dying so we can reap them.
  pids = os::pids();
  if (pids.isError()) {
    return process::Failure("Failed to list of processes");
  }

  std::list<process::Future<Option<int>>> futures;

  foreach (pid_t pid, pids.get()) {
    Try<ino_t> ns = ns::getns(pid, "pid");
    if (ns.isSome() && ns.get() == inode) {
      futures.push_back(process::reap(pid));
    }

    // Ignore any errors, probably because the process no longer
    // exists, and ignorable otherwise.
  }

  // Wait for all the signalled processes to terminate. The pid
  // namespace will then be empty and will be released by the kernel
  // (unless there are additional references).
  return process::collect(futures)
    .then([]() { return Nothing(); });
}

} // namespace pid {


// Returns the namespace flags in the string form of bitwise-ORing the
// flags, e.g., CLONE_NEWNS | CLONE_NEWNET.
inline std::string stringify(int flags)
{
  hashmap<unsigned int, std::string> names = {
    {CLONE_NEWNS,   "CLONE_NEWNS"},
    {CLONE_NEWUTS,  "CLONE_NEWUTS"},
    {CLONE_NEWIPC,  "CLONE_NEWIPC"},
    {CLONE_NEWPID,  "CLONE_NEWPID"},
    {CLONE_NEWNET,  "CLONE_NEWNET"},
    {CLONE_NEWUSER, "CLONE_NEWUSER"},
    {CLONE_NEWCGROUP, "CLONE_NEWCGROUP"}
  };

  std::vector<std::string> namespaces;
  foreachpair (unsigned int flag, const std::string& name, names) {
    if (flags & flag) {
      namespaces.push_back(name);
    }
  }

  return strings::join(" | ", namespaces);
}

} // namespace ns {

#endif // __LINUX_NS_HPP__
