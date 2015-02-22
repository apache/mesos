/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/mount.h>

#include <list>
#include <set>
#include <string>

#include <stout/os.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/ls.hpp>

#include "linux/fs.hpp"
#include "linux/ns.hpp"

#include "slave/containerizer/isolators/namespaces/pid.hpp"

using namespace process;

using std::list;
using std::set;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

using mesos::slave::ExecutorRunState;
using mesos::slave::Isolator;
using mesos::slave::IsolatorProcess;
using mesos::slave::Limitation;

// The root directory where we bind mount all the namespace handles.
const string BIND_MOUNT_ROOT = "/var/run/mesos/pidns";

// The empty directory that we'll use to mask the namespace handles
// inside each container. This mount ensures they cannot determine the
// namespace of another container.
const string BIND_MOUNT_MASK_DIR = "/var/empty/mesos";


// Helper to construct the path to a pid's namespace file.
inline string nsProcFile(pid_t pid)
{
  return path::join("/proc", stringify(pid), "ns", "pid");
}


// Helper to construct the path to the additional reference created
// for a container's pid namespace.
inline string nsExtraReference(const ContainerID& containerId)
{
  return path::join(BIND_MOUNT_ROOT, stringify(containerId));
}

Try<Isolator*> NamespacesPidIsolatorProcess::create(const Flags& flags)
{
  // Check for root permission.
  if (geteuid() != 0) {
    return Error("The pid namespace isolator requires root permissions");
  }

  // Verify that pid namespaces are available on this kernel.
  if (ns::namespaces().count("pid") == 0) {
    return Error("Pid namespaces are not supported by this kernel");
  }

  // Create the directory where bind mounts of the pid namespace will
  // be placed.
  Try<Nothing> mkdir = os::mkdir(BIND_MOUNT_ROOT);
  if (mkdir.isError()) {
    return Error(
        "Failed to create the bind mount root directory at " +
        BIND_MOUNT_ROOT + ": " + mkdir.error());
  }

  // Create the empty directory that will be used to mask the bind
  // mounts inside each container.
  mkdir = os::mkdir(BIND_MOUNT_MASK_DIR);
  if (mkdir.isError()) {
    return Error(
        "Failed to create the bind mount mask direcrory at " +
        BIND_MOUNT_MASK_DIR + ": " + mkdir.error());
  }

  return new Isolator(Owned<IsolatorProcess>(
      new NamespacesPidIsolatorProcess()));
}


Result<ino_t> NamespacesPidIsolatorProcess::getNamespace(
    const ContainerID& containerId)
{
  const string target = nsExtraReference(containerId);

  if (os::exists(target)) {
    struct stat s;
    if (::stat(target.c_str(), &s) < 0) {
      return ErrnoError("Failed to stat namespace reference");
    }

    return s.st_ino;
  }

  return None();
}


Future<Nothing> NamespacesPidIsolatorProcess::recover(
    const list<ExecutorRunState>& states)
{
  hashset<ContainerID> containers;

  foreach (const ExecutorRunState& state, states) {
    containers.insert(state.id);
  }

  // Clean up any orphaned bind mounts and empty files.
  Try<list<string> > entries = os::ls(BIND_MOUNT_ROOT);
  if (entries.isError()) {
    return Failure("Failed to list existing containers in '" +
                   BIND_MOUNT_ROOT + "': " + entries.error());
  }

  foreach (const string& entry, entries.get()) {
    ContainerID containerId;
    containerId.set_value(entry);

    if (!containers.contains(containerId)) {
      cleanup(containerId);
    }
  }

  return Nothing();
}


Future<Option<CommandInfo> > NamespacesPidIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& user)
{
  list<string> commands;

  // Mask the bind mount root directory in each container so
  // containers cannot see the namespace bind mount of other
  // containers.
  commands.push_back(
      "mount -n --bind " + BIND_MOUNT_MASK_DIR + " " + BIND_MOUNT_ROOT);

  // Mount /proc for the container's pid namespace to show the
  // container's pids (and other /proc files), not the parent's. We
  // first recursively make the mount private because /proc is usually
  // marked explicitly as shared (see /proc/self/mountinfo) and
  // changes would propagate to the parent's /proc mount otherwise. We
  // then mount /proc with the standard options. This technique was
  // taken from unshare.c in utils-linux for --mount-proc. We use the
  // -n flag so the mount is not added to the mtab where it will not
  // be correctly removed with the namespace terminates.
  commands.push_back("mount none /proc --make-private -o rec");
  commands.push_back("mount -n -t proc proc /proc -o nosuid,noexec,nodev");

  CommandInfo command;
  command.set_value(strings::join(" && ", commands));

  return command;
}


Future<Nothing> NamespacesPidIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  const string source = nsProcFile(pid);
  const string target = nsExtraReference(containerId);

  // Create a bind mount of the pid namespace so we can control the
  // lifetime of the pid namespace. This lets us identify the
  // container's pid namespace, even if the leading pid has exited.
  Try<Nothing> touch = os::touch(target);
  if (touch.isError()) {
    return Failure("Failed to create bind mount target: " + touch.error());
  }

  Try<Nothing> mount = fs::mount(source, target, "none", MS_BIND, NULL);
  if (mount.isError()) {
    return Failure(
        "Failed to mount pid namespace handle from " +
        source + " to " + target + ": " + mount.error());
  }

  return Nothing();
}


Future<Limitation> NamespacesPidIsolatorProcess::watch(
    const ContainerID& containerId)
{
  return Future<Limitation>();
}


Future<Nothing> NamespacesPidIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return Nothing();
}


Future<ResourceStatistics> NamespacesPidIsolatorProcess::usage(
    const ContainerID& containerId)
{
  return ResourceStatistics();
}


// An old glibc might not have this symbol.
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif


Future<Nothing> NamespacesPidIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  const string target = nsExtraReference(containerId);

  if (os::exists(target)) {
    // We don't expect anyone to have a reference to target but do a
    // lazy umount in case. We do not want to force the umount; it
    // will not cause an issue if this umount is delayed.
    Try<Nothing> unmount = fs::unmount(target, MNT_DETACH);

    // This will fail if the unmount hasn't completed yet but this
    // only leaks a uniquely named empty file that will cleaned up as
    // an orphan on recovery.
    os::rm(target);
  }

  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
