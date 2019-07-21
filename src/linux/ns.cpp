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

#include "linux/ns.hpp"

#include <unistd.h>

#include <sys/socket.h>
#include <sys/wait.h>

#include <cstring>
#include <type_traits>
#include <vector>

#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/reap.hpp>

#include <stout/assert.hpp>
#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/proc.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/version.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/ls.hpp>
#include <stout/os/socket.hpp>

#include "common/kernel_version.hpp"
#include "common/status_utils.hpp"

using std::set;
using std::string;
using std::vector;

namespace ns {

Try<int> nstype(const string& ns)
{
  const hashmap<string, int> nstypes = {
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


Try<string> nsname(int nsType)
{
  const hashmap<int, string> nsnames = {
    {CLONE_NEWNS, "mnt"},
    {CLONE_NEWUTS, "uts"},
    {CLONE_NEWIPC, "ipc"},
    {CLONE_NEWNET, "net"},
    {CLONE_NEWUSER, "user"},
    {CLONE_NEWPID, "pid"},
    {CLONE_NEWCGROUP, "cgroup"}
  };

  Option<string> nsname = nsnames.get(nsType);

  if (nsname.isNone()) {
    return Error("Unknown namespace");
  }

  return nsname.get();
}


// TODO(jpeach): As we move namespace parameters from strings to CLONE
// constants, we should be able to eventually remove the internal uses
// of this function.
static set<string> namespaces()
{
  set<string> result;

  Try<std::list<string>> entries = os::ls("/proc/self/ns");
  if (entries.isSome()) {
    foreach (const string& entry, entries.get()) {
      // Introduced in Linux 4.12, pid_for_children is a handle for the PID
      // namespace of child processes created by the current process.
      if (entry != "pid_for_children") {
        result.insert(entry);
      }
    }
  }

  return result;
}


set<int> nstypes()
{
  set<int> result;

  foreach (const string& ns, namespaces()) {
    Try<int> type = nstype(ns);
    if (type.isSome()) {
      result.insert(type.get());
    }
  }

  return result;
}


Try<bool> supported(int nsTypes)
{
  int supported = 0;

  foreach (const int n, nstypes()) {
    if (nsTypes & n) {
      supported |= n;
    }
  }

  if ((nsTypes & CLONE_NEWUSER) && (supported & CLONE_NEWUSER)) {
    Try<Version> version = mesos::kernelVersion();

    if (version.isError()) {
      return Error(version.error());
    }

    if (version.get() < Version(3, 12, 0)) {
      return false;
    }
  }

  return supported == nsTypes;
}


Try<Nothing> setns(
    const string& path,
    const string& ns,
    bool checkMultithreaded)
{
  if (checkMultithreaded) {
    // Return error if there're multiple threads in the calling process.
    Try<set<pid_t>> threads = proc::threads(::getpid());
    if (threads.isError()) {
      return Error(
          "Failed to get the threads of the current process: " +
          threads.error());
    } else if (threads->size() > 1) {
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


Try<Nothing> setns(pid_t pid, const string& ns, bool checkMultithreaded)
{
  if (!os::exists(pid)) {
    return Error("Pid " + ::stringify(pid) + " does not exist");
  }

  string path = path::join("/proc", ::stringify(pid), "ns", ns);
  if (!os::exists(path)) {
    return Error("Namespace '" + ns + "' is not supported");
  }

  return ns::setns(path, ns, checkMultithreaded);
}


Result<ino_t> getns(pid_t pid, const string& ns)
{
  if (ns::namespaces().count(ns) < 1) {
    return Error("Namespace '" + ns + "' is not supported");
  }

  string path = path::join("/proc", ::stringify(pid), "ns", ns);
  struct stat s;
  if (::stat(path.c_str(), &s) < 0) {
    if (errno == ENOENT) {
      // Process is gone.
      return None();
    } else {
      return ErrnoError("Failed to stat " + ns + " namespace handle"
                        " for pid " + ::stringify(pid));
    }
  }

  return s.st_ino;
}


// Helper for closing a container of file descriptors.
template <
  typename Iterable,
  typename = typename std::enable_if<
    std::is_same<typename Iterable::value_type, int>::value>::type>
static void close(const Iterable& fds)
{
  int errsav = errno;

  foreach (int fd, fds) {
    ::close(fd); // Need to call the async-signal safe version.
  }

  errno = errsav;
}


Try<pid_t> clone(
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
  const struct
  {
    int nstype;
    string name;
  } namespaces[NAMESPACES] = {
    {CLONE_NEWUSER, "user"},
    {CLONE_NEWCGROUP, "cgroup"},
    {CLONE_NEWIPC, "ipc"},
    {CLONE_NEWUTS, "uts"},
    {CLONE_NEWNET, "net"},
    {CLONE_NEWPID, "pid"},
    {CLONE_NEWNS, "mnt"}
  };

  // Since we assume below that the parent can deallocate the stack
  // after cloning the children, the caller must not pass CLONE_VM.
  // That would cause the both processes to share their address space
  // so deallocating the stack in the parent would affect the child.
  CHECK_EQ(0, flags & CLONE_VM);

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

  // NOTE: we do all of this ahead of time so we can be async signal
  // safe after calling fork below.
  for (size_t i = 0; i < NAMESPACES; i++) {
    // Only open the namespace file descriptor if it's been requested.
    if (namespaces[i].nstype & nstypes) {
      const string path =
        path::join("/proc", ::stringify(target), "ns", namespaces[i].name);
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
  Try<std::array<int_fd, 2>> sockets = net::socketpair(AF_UNIX, SOCK_STREAM, 0);
  if (sockets.isError()) {
    close(fds.values());
    return Error("Failed to create Unix domain socket: " + sockets.error());
  }

  // Need to set SO_PASSCRED option in order to receive credentials
  // (which is how we get the pid of the clone'd process, see
  // below). Note that apparently we only need to do this for
  // receiving, not also for sending.
  const int value = 1;
  const socklen_t size = sizeof(value);
  if (setsockopt(sockets->at(0), SOL_SOCKET, SO_PASSCRED, &value, size) == -1) {
    close(fds.values());
    close(sockets.get());
    return ErrnoError("Failed to set socket option SO_PASSCRED");
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

  iovec iov = {nullptr};
  iov.iov_base = base;
  iov.iov_len = sizeof(base);

  // We need to allocate a char array large enough to hold "control" data.
  // However, since this buffer is in reality a 'cmsghdr' with the payload, we
  // use a union to ensure that it is aligned as required for that structure.
  union {
    cmsghdr cmessage;
    char control[CMSG_SPACE(sizeof(ucred))];
  };

  cmessage.cmsg_len = CMSG_LEN(sizeof(ucred));
  cmessage.cmsg_level = SOL_SOCKET;
  cmessage.cmsg_type = SCM_CREDENTIALS;

  msghdr message = {nullptr};
  message.msg_name = nullptr;
  message.msg_namelen = 0;
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control); // CMSG_LEN(sizeof(ucred));

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
    close(sockets.get());
    return ErrnoError();
  } else if (child > 0) {
    // Parent.
    stack->deallocate();

    close(fds.values());
    ::close(sockets->at(1));

    ssize_t length = recvmsg(sockets->at(0), &message, 0);

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
      ::close(sockets->at(0));
      kill(child, SIGKILL);
      return error;
    } else if (length == 0) {
      // Socket closed, child must have died, but kill anyway.
      ::close(sockets->at(0));
      kill(child, SIGKILL);
      return Error("Failed to receive: Socket closed");
    }

    ::close(sockets->at(0));

    // Extract pid.
    if (CMSG_FIRSTHDR(&message) == nullptr ||
        CMSG_FIRSTHDR(&message)->cmsg_len != CMSG_LEN(sizeof(ucred)) ||
        CMSG_FIRSTHDR(&message)->cmsg_level != SOL_SOCKET ||
        CMSG_FIRSTHDR(&message)->cmsg_type != SCM_CREDENTIALS) {
      kill(child, SIGKILL);
      return Error("Bad control data received");
    }

    ucred cred;
    std::memcpy(&cred, CMSG_DATA(CMSG_FIRSTHDR(&message)), sizeof(ucred));

    const pid_t pid = cred.pid;

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
    ::close(sockets->at(0));

    // Loop through and 'setns' into all of the parent namespaces that
    // have been requested.
    for (size_t i = 0; i < NAMESPACES; i++) {
      Option<int> fd = fds.get(namespaces[i].nstype);
      if (fd.isSome()) {
        ASSERT(namespaces[i].nstype & nstypes);
        if (::setns(fd.get(), namespaces[i].nstype) < 0) {
          close(fds.values());
          ::close(sockets->at(1));
          _exit(EXIT_FAILURE);
        }
      }
    }

    close(fds.values());

    auto grandchildMain = [=]() -> int {
      // Grandchild (second child, now completely entered in the
      // namespaces of the target).
      //
      // Now clone with the specified flags, close the unused socket,
      // and execute the specified function.
      pid_t pid = os::signal_safe::clone(
          stack.get(),
          flags,
          [=]() {
            ucred cred;
            cred.pid = ::getpid();
            cred.uid = ::getuid();
            cred.gid = ::getgid();

            // Now send back the pid and have it be translated appropriately
            // by the kernel to the enclosing pid namespace.
            //
            // NOTE: sending back the pid is best effort because we're going
            // to exit no matter what.
            std::memcpy(
                CMSG_DATA(CMSG_FIRSTHDR(&message)), &cred, sizeof(ucred));

            if (sendmsg(sockets->at(1), &message, 0) == -1) {
              // Failed to send the pid back to the parent!
              _exit(EXIT_FAILURE);
            }

            ::close(sockets->at(1));

            return f();
          });

      ::close(sockets->at(1));

      // TODO(benh): Kill ourselves with an exit status that we can
      // decode above to determine why `clone` failed.
      _exit(pid < 0 ? EXIT_FAILURE : EXIT_SUCCESS);
      UNREACHABLE();
    };

    os::Stack grandchildStack(os::Stack::DEFAULT_SIZE);

    if (!grandchildStack.allocate()) {
      ::close(sockets->at(1));
      _exit(EXIT_FAILURE);
    }

    // Fork again to make sure we're actually in those namespaces
    // (required for the pid namespace at least).
    //
    // NOTE: We use clone instead of fork here because of a glibc bug.
    // glibc version < 2.25 has an assertion in 'fork()' which checks
    // if the child process's pid is not the same as the parent. This
    // invariant is no longer true with pid namespaces being
    // introduced. See more details in MESOS-7858.
    //
    // NOTE: glibc 'fork()' also specifies 'CLONE_CHILD_SETTID' and
    // 'CLONE_CHILD_CLEARTID' for the clone flags. However, since we
    // are not using any pthread library in the grandchild, we don't
    // need those flags.
    //
    // TODO(benh): Don't do a fork if we're not actually entering the
    // PID namespace since the extra fork is unnecessary.
    pid_t grandchild =
      os::signal_safe::clone(grandchildStack, SIGCHLD, grandchildMain);

    grandchildStack.deallocate();

    if (grandchild < 0) {
      // TODO(benh): Exit with `errno` in order to capture `fork` error?
      ::close(sockets->at(1));
      _exit(EXIT_FAILURE);
    } else if (grandchild > 0) {
      // Still the (first) child.
      ::close(sockets->at(1));

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

      ASSERT(WIFEXITED(status) || WIFSIGNALED(status));

      if (WIFEXITED(status)) {
        _exit(WEXITSTATUS(status));
      }

      ASSERT(WIFSIGNALED(status));
      raise(WTERMSIG(status));
    }
  }
  UNREACHABLE();
}


string stringify(int flags)
{
  const hashmap<unsigned int, string> names = {
    {CLONE_NEWNS,   "CLONE_NEWNS"},
    {CLONE_NEWUTS,  "CLONE_NEWUTS"},
    {CLONE_NEWIPC,  "CLONE_NEWIPC"},
    {CLONE_NEWPID,  "CLONE_NEWPID"},
    {CLONE_NEWNET,  "CLONE_NEWNET"},
    {CLONE_NEWUSER, "CLONE_NEWUSER"},
    {CLONE_NEWCGROUP, "CLONE_NEWCGROUP"}
  };

  vector<string> namespaces;
  foreachpair (unsigned int flag, const string& name, names) {
    if (flags & flag) {
      namespaces.push_back(name);
    }
  }

  return strings::join(" | ", namespaces);
}

} // namespace ns {
