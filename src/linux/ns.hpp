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

#include <sched.h>

#include <sys/syscall.h>

#include <set>
#include <string>

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

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
Try<int> nstype(const std::string& ns);


// Given a single CLONE_NEW* constant, return the corresponding namespace
// name. This is the inverse of ns::nstype().
Try<std::string> nsname(int nsType);


// Returns all the configured kernel namespaces.
std::set<int> nstypes();


// Returns true if all the given CLONE_NEW* constants are supported
// in the running kernel. If CLONE_NEWUSER is specified, the kernel
// version must be at least 3.12.0 since prior to that version, major
// kernel subsystems (e.g. XFS) did not implement user namespace
// support. See also user_namespaces(7).
Try<bool> supported(int nsTypes);


// Re-associate the calling process with the specified namespace. The
// path refers to one of the corresponding namespace entries in the
// /proc/[pid]/ns/ directory (or bind mounted elsewhere). We do not
// allow a process with multiple threads to call this function because
// it will lead to some weird situations where different threads of a
// process are in different namespaces.
Try<Nothing> setns(
    const std::string& path,
    const std::string& ns,
    bool checkMultithreaded = true);


// Re-associate the calling process with the specified namespace. The
// pid specifies the process whose namespace we will associate.
Try<Nothing> setns(
    pid_t pid,
    const std::string& ns,
    bool checkMultithreaded = true);


// Get the inode number of the specified namespace for the specified
// pid. The inode number identifies the namespace and can be used for
// comparisons, i.e., two processes with the same inode for a given
// namespace type are in the same namespace.
Result<ino_t> getns(pid_t pid, const std::string& ns);


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
Try<pid_t> clone(
    pid_t target,
    int nstypes,
    const lambda::function<int()>& f,
    int flags);


// Returns the namespace flags in the string form of bitwise-ORing the
// flags, e.g., CLONE_NEWNS | CLONE_NEWNET.
std::string stringify(int flags);

} // namespace ns {

#endif // __LINUX_NS_HPP__
