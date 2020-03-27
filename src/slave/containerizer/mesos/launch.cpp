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

#include <errno.h>
#ifdef __linux__
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#endif // __linux__
#include <string.h>

#include <algorithm>
#include <iostream>
#include <set>
#include <string>

#include <glog/logging.h>
#include <glog/raw_logging.h>

#include <process/subprocess.hpp>

#include <stout/adaptor.hpp>
#include <stout/foreach.hpp>
#include <stout/fs.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/exec.hpp>
#include <stout/os/int_fd.hpp>
#include <stout/os/open.hpp>
#include <stout/os/realpath.hpp>
#include <stout/os/shell.hpp>
#include <stout/os/which.hpp>
#include <stout/os/write.hpp>

#ifdef __WINDOWS__
#include <stout/os/windows/jobobject.hpp>
#endif // __WINDOWS__

#include <mesos/mesos.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/slave/containerizer.hpp>

#include "common/parse.hpp"
#include "common/status_utils.hpp"

#ifdef __linux__
#include "linux/capabilities.hpp"
#include "linux/fs.hpp"
#include "linux/ns.hpp"
#endif

#ifdef ENABLE_SECCOMP_ISOLATOR
#include "linux/seccomp/seccomp.hpp"
#endif

#ifndef __WINDOWS__
#include "posix/rlimits.hpp"
#endif // __WINDOWS__

#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/paths.hpp"

using std::cerr;
using std::endl;
using std::set;
using std::string;
using std::vector;

using process::Owned;

#ifdef __linux__
using mesos::internal::capabilities::Capabilities;
using mesos::internal::capabilities::Capability;
using mesos::internal::capabilities::ProcessCapabilities;
#endif // __linux__

#ifdef ENABLE_SECCOMP_ISOLATOR
using mesos::internal::seccomp::SeccompFilter;
#endif

using mesos::slave::ContainerFileOperation;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerMountInfo;

namespace mesos {
namespace internal {
namespace slave {

const string MesosContainerizerLaunch::NAME = "launch";


MesosContainerizerLaunch::Flags::Flags()
{
  add(&Flags::launch_info,
      "launch_info",
      "");

  add(&Flags::pipe_read,
      "pipe_read",
      "The read end of the control pipe. This is a file descriptor \n"
      "on Posix, or a handle on Windows. It's caller's responsibility \n"
      "to make sure the file descriptor or the handle is inherited \n"
      "properly in the subprocess. It's used to synchronize with the \n"
      "parent process. If not specified, no synchronization will happen.");

  add(&Flags::pipe_write,
      "pipe_write",
      "The write end of the control pipe. This is a file descriptor \n"
      "on Posix, or a handle on Windows. It's caller's responsibility \n"
      "to make sure the file descriptor or the handle is inherited \n"
      "properly in the subprocess. It's used to synchronize with the \n"
      "parent process. If not specified, no synchronization will happen.");

  add(&Flags::runtime_directory,
      "runtime_directory",
      "The runtime directory for the container (used for checkpointing)");

#ifdef __linux__
  add(&Flags::namespace_mnt_target,
      "namespace_mnt_target",
      "The target 'pid' of the process whose mount namespace we'd like\n"
      "to enter before executing the command.");

  add(&Flags::unshare_namespace_mnt,
      "unshare_namespace_mnt",
      "Whether to launch the command in a new mount namespace.",
      false);
#endif // __linux__
}


static Option<pid_t> containerPid = None();
static Option<string> containerStatusPath = None();
static Option<int_fd> containerStatusFd = None();

static void exitWithSignal(int sig);
static void exitWithStatus(int status);


#ifndef __WINDOWS__
static void signalSafeWriteStatus(int status)
{
  const string statusString = std::to_string(status);

  ssize_t result =
    os::signal_safe::write(containerStatusFd.get(), statusString);

  if (result < 0) {
    // NOTE: We use RAW_LOG instead of LOG because RAW_LOG doesn't
    // allocate any memory or grab locks. And according to
    // https://code.google.com/p/google-glog/issues/detail?id=161
    // it should work in 'most' cases in signal handlers.
    RAW_LOG(ERROR, "Failed to write container status '%d': %d", status, errno);
  }
}


// When launching the executor with an 'init' process, we need to
// forward all relevant signals to it. The functions below help to
// enable this forwarding.
static void signalHandler(int sig)
{
  // If we don't yet have a container pid, we treat
  // receiving a signal like a failure and exit.
  if (containerPid.isNone()) {
    exitWithSignal(sig);
  }

  // Otherwise we simply forward the signal to `containerPid`. We
  // purposefully ignore the error here since we have to remain async
  // signal safe. The only possible error scenario relevant to us is
  // ESRCH, but if that happens that means our pid is already gone and
  // the process will exit soon. So we are safe.
  os::kill(containerPid.get(), sig);
}


static Try<Nothing> installSignalHandlers()
{
  // Install handlers for all standard POSIX signals
  // (i.e. any signal less than `NSIG`).
  for (int i = 1; i < NSIG; i++) {
    // We don't want to forward the SIGCHLD signal, nor do we want to
    // handle it ourselves because we reap all children inline in the
    // `execute` function.
    if (i == SIGCHLD) {
      continue;
    }

    // We can't catch or ignore these signals, so we shouldn't try
    // to register a handler for them.
    if (i == SIGKILL || i == SIGSTOP) {
      continue;
    }

    // The NSIG constant is used to determine the number of signals
    // available on a system. However, Darwin, Linux, and BSD differ
    // on their interpretation of of the value of NSIG. Linux, for
    // example, sets it to 65, where Darwin sets it to 32. The reason
    // for the discrepancy is that Linux includes the real-time
    // signals in this count, where Darwin does not. However, even on
    // linux, we are not able to arbitrarily install signal handlers
    // for all the real-time signals -- they must have not been
    // registered with the system first. For this reason, we
    // standardize on verifying the installation of handlers for
    // signals 1-31 (since these are defined in the POSIX standard),
    // but we continue to attempt to install handlers up to the value
    // of NSIG without verification.
    const int posixLimit = 32;
    if (os::signals::install(i, signalHandler) != 0 && i < posixLimit) {
      return ErrnoError("Unable to register signal"
                        " '" + stringify(strsignal(i)) + "'");
    }
  }

  return Nothing();
}
#endif // __WINDOWS__


static void exitWithSignal(int sig)
{
#ifndef __WINDOWS__
  if (containerStatusFd.isSome()) {
    signalSafeWriteStatus(W_EXITCODE(0, sig));
    os::close(containerStatusFd.get());
  }
#endif // __WINDOWS__
  ::_exit(EXIT_FAILURE);
}


static void exitWithStatus(int status)
{
#ifndef __WINDOWS__
  if (containerStatusFd.isSome()) {
    signalSafeWriteStatus(W_EXITCODE(status, 0));
    os::close(containerStatusFd.get());
  }
#endif // __WINDOWS__
  ::_exit(status);
}


#ifdef __linux__
static Try<Nothing> mountContainerFilesystem(const ContainerMountInfo& mount)
{
  return fs::mount(
      mount.has_source() ? Option<string>(mount.source()) : None(),
      mount.target(),
      mount.has_type() ? Option<string>(mount.type()) : None(),
      mount.has_flags() ? mount.flags() : 0,
      mount.has_options() ? Option<string>(mount.options()) : None());
}
#endif // __linux__


static Try<Nothing> prepareMounts(const ContainerLaunchInfo& launchInfo)
{
#ifdef __linux__
  bool cloneMountNamespace = std::find(
      launchInfo.clone_namespaces().begin(),
      launchInfo.clone_namespaces().end(),
      CLONE_NEWNS) != launchInfo.clone_namespaces().end();

  if (!cloneMountNamespace) {
    // Mounts are not supported if the mount namespace is not cloned.
    // Otherwise, we'll pollute the parent mount namespace.
    if (!launchInfo.mounts().empty()) {
      return Error(
          "Mounts are not supported if the mount namespace is not cloned");
    }

    return Nothing();
  }

  // Now, setup the mount propagation for the container.
  //   1) If there is no shared mount (i.e., "bidirectional"
  //      propagation), mark the root as recursively slave propagation
  //      (i.e., --make-rslave) so that mounts do not leak to parent
  //      mount namespace.
  //   2) If there exist shared mounts, scan the mount table and mark
  //      the rest as shared mounts one by one.
  //
  // TODO(jieyu): Currently, if the container has its own rootfs, the
  // 'fs::chroot::enter' function will mark `/` as recursively slave.
  // This will cause problems for shared mounts. As a result,
  // bidirectional mount propagation does not work for containers that
  // have rootfses.
  //
  // TODO(jieyu): Another caveat right now is that the CNI isolator
  // will mark `/` as recursively slave in `isolate()` method if the
  // container joins a named network. As a result, bidirectional mount
  // propagation currently does not work for containers that want to
  // join a CNI network.
  bool hasSharedMount = std::find_if(
      launchInfo.mounts().begin(),
      launchInfo.mounts().end(),
      [](const ContainerMountInfo& mount) {
        return (mount.flags() & MS_SHARED) != 0;
      }) != launchInfo.mounts().end();

  if (!hasSharedMount) {
    Try<Nothing> mnt =
      fs::mount(None(), "/", None(), MS_SLAVE | MS_REC, None());

    if (mnt.isError()) {
      return Error("Failed to mark '/' as rslave: " + mnt.error());
    }

    cerr << "Marked '/' as rslave" << endl;
  } else {
    hashset<string> sharedMountTargets;
    foreach (const ContainerMountInfo& mount, launchInfo.mounts()) {
      // Skip normal mounts.
      if ((mount.flags() & MS_SHARED) == 0) {
        continue;
      }

      sharedMountTargets.insert(mount.target());
    }

    Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
    if (table.isError()) {
      return Error("Failed to get mount table: " + table.error());
    }

    foreach (const fs::MountInfoTable::Entry& entry,
             adaptor::reverse(table->entries)) {
      if (!sharedMountTargets.contains(entry.target)) {
        Try<Nothing> mnt = fs::mount(
            None(),
            entry.target,
            None(),
            MS_SLAVE,
            None());

        if (mnt.isError()) {
          return Error(
              "Failed to mark '" + entry.target +
              "' as slave: " + mnt.error());
        }
      }
    }
  }

  foreach (const ContainerMountInfo& mount, launchInfo.mounts()) {
    // Skip those mounts that are used for setting up propagation.
    if ((mount.flags() & MS_SHARED) != 0) {
      continue;
    }

    // If bidirectional mount exists, we will not mark `/` as
    // recursively slave (otherwise, the bidirectional mount
    // propagation won't work).
    //
    // At the same time, we want to prevent mounts in the child
    // process from being propagated to the host mount namespace,
    // except for the ones that set the propagation mode to be
    // bidirectional. This ensures a clean host mount table, and
    // greatly simplifies the container cleanup.
    //
    // If the target of a volume mount is under a non shared mount,
    // the mount won't be propagated to the host mount namespace,
    // which is what we want. Otherwise, the volume mount will be
    // propagated to the host mount namespace, which will make proper
    // cleanup almost impossible. Therefore, we perform a sanity check
    // here to make sure the propagation to host mount namespace does
    // not happen.
    //
    // One implication of this check is that: if the target of a
    // volume mount is under a mount that has to be shared (e.g.,
    // explicitly specified by the user using 'MountPropagation' in
    // HOST_PATH volume), the volume mount will fail.
    //
    // TODO(jieyu): Some isolators are still using `pre_exe_commands`
    // to do mounts. Those isolators thus will escape this check. We
    // should consider forcing all isolators to use
    // `ContainerMountInfo` for volume mounts.
    if (hasSharedMount) {
      Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
      if (table.isError()) {
        return Error("Failed to read mount table: " + table.error());
      }

      Try<fs::MountInfoTable::Entry> entry =
        table->findByTarget(mount.target());

      if (entry.isError()) {
        return Error(
            "Cannot find the mount containing the mount target '" +
            mount.target() + "': " + entry.error());
      }

      if (entry->shared().isSome()) {
        return Error(
            "Cannot perform mount '" + stringify(JSON::protobuf(mount)) +
            "' because the target is under a shared mount "
            "'" + entry->target + "'");
      }
    }

    // If the mount target doesn't exist yet, create it. The isolator
    // is responsible for ensuring the target path is safe.
    if (mount.has_source() && !os::exists(mount.target())) {
      const string dirname = Path(mount.target()).dirname();

      if (!os::exists(dirname)) {
        Try<Nothing> mkdir = os::mkdir(Path(mount.target()).dirname());

        if (mkdir.isError()) {
          return Error(
              "Failed to create mount target directory '" + dirname + "': " +
              mkdir.error());
        }
      }

      // The mount source could be a file, a directory, or a Linux
      // pseudo-filesystem. In the last case, the target must be a
      // directory, so if the source doesn't exist, we default to
      // mounting on a directory.
      Try<Nothing> target =
        (!os::exists(mount.source()) || os::stat::isdir(mount.source()))
          ? os::mkdir(mount.target())
          : os::touch(mount.target());

      if (target.isError()) {
        return Error(
            "Failed to create mount target '" + mount.target() + "': " +
            target.error());
      }
    }

    Try<Nothing> mnt = mountContainerFilesystem(mount);
    if (mnt.isError()) {
      return Error(
          "Failed to mount '" + stringify(JSON::protobuf(mount)) +
          "': " + mnt.error());
    }
  }
#endif // __linux__

  return Nothing();
}


static Try<Nothing> maskPath(const string& target)
{
  Try<Nothing> mnt = Nothing();

#ifdef __linux__
  if (os::stat::isfile(target)) {
    mnt = fs::mount("/dev/null", target, None(), MS_BIND | MS_RDONLY, None());
  } else if (os::stat::isdir(target)) {
    mnt = fs::mount(None(), target, "tmpfs", MS_RDONLY, "size=0");
  }
#endif // __linux__

  if (mnt.isError()) {
    return Error("Failed to mask '" + target + "': " + mnt.error());
  }

  return Nothing();
}


static Try<Nothing> executeFileOperation(const ContainerFileOperation& op)
{
  switch (op.operation()) {
    case ContainerFileOperation::SYMLINK: {
      Try<Nothing> result =
        ::fs::symlink(op.symlink().source(), op.symlink().target());
      if (result.isError()) {
        return Error(
            "Failed to link '" + op.symlink().source() + "' as '" +
            op.symlink().target() + "': " + result.error());
      }

      return Nothing();
    }

    case ContainerFileOperation::MOUNT: {
#ifdef __linux__
      Try<Nothing> result = mountContainerFilesystem(op.mount());
      if (result.isError()) {
        return Error(
            "Failed to mount '" + stringify(JSON::protobuf(op.mount())) +
            "': " + result.error());
      }

      return Nothing();
#else
      return Error("Container mount is not supported on non-Linux systems");
#endif // __linux__
    }

    case ContainerFileOperation::RENAME: {
      Try<Nothing> result =
        os::rename(op.rename().source(), op.rename().target());

      // TODO(jpeach): We should only fall back to `mv` if the error
      // is EXDEV, in which case a rename is a copy+unlink.
      if (result.isError()) {
        Option<int> status = os::spawn(
            "mv", {"mv", "-f", op.rename().source(), op.rename().target()});

        if (status.isNone()) {
          return Error(
              "Failed to rename '" + op.rename().source() + "' to '" +
              op.rename().target() + "':  spawn failed");
        }

        if (!WSUCCEEDED(status.get())) {
          return Error(
              "Failed to rename '" + op.rename().source() + "' to '" +
              op.rename().target() + "': " + WSTRINGIFY(status.get()));
        }

        result = Nothing();
      }

      return Nothing();
  }

    case ContainerFileOperation::MKDIR: {
      Try<Nothing> result =
        os::mkdir(op.mkdir().target(), op.mkdir().recursive());
      if (result.isError()) {
        return Error(
            "Failed to create directory '" + op.mkdir().target() + "': " +
            result.error());
      }

      return result;
    }
  }

  return Nothing();
}


static Try<Nothing> installResourceLimits(const RLimitInfo& limits)
{
#ifdef __WINDOWS__
  return Error("Rlimits are not supported on Windows");
#else
  foreach (const RLimitInfo::RLimit& limit, limits.rlimits()) {
    Try<Nothing> set = rlimits::set(limit);
    if (set.isError()) {
      return Error(
          "Failed to set " +
          RLimitInfo::RLimit::Type_Name(limit.type()) + " limit: " +
          set.error());
    }
  }

  return Nothing();
#endif // __WINDOWS__
}


static Try<Nothing> enterChroot(const string& rootfs)
{
#ifdef __WINDOWS__
  return Error("Changing rootfs is not supported on Windows");
#else
#ifdef __linux__
  Try<Nothing> chroot = fs::chroot::enter(rootfs);
#else
  // For any other platform we'll just use POSIX chroot.
  Try<Nothing> chroot = os::chroot(rootfs);
#endif // __linux__

  if (chroot.isError()) {
    return Error(
        "Failed to enter chroot '" + rootfs + "': " +
        chroot.error());
  }

  return Nothing();
#endif // __WINDOWS__
}


#ifdef __linux__
static void calculateCapabilities(
    const ContainerLaunchInfo& launchInfo,
    ProcessCapabilities* capabilities)
{
  // If the task has any effective capabilities, grant them to all
  // the capability sets.
  if (launchInfo.has_effective_capabilities()) {
    set<Capability> target =
      capabilities::convert(launchInfo.effective_capabilities());

    capabilities->set(capabilities::AMBIENT, target);
    capabilities->set(capabilities::EFFECTIVE, target);
    capabilities->set(capabilities::PERMITTED, target);
    capabilities->set(capabilities::INHERITABLE, target);
    capabilities->set(capabilities::BOUNDING, target);
  }

  // If we also have bounding capabilities, apply that in preference to
  // the effective capabilities.
  if (launchInfo.has_bounding_capabilities()) {
    set<Capability> bounding =
      capabilities::convert(launchInfo.bounding_capabilities());

    capabilities->set(capabilities::BOUNDING, bounding);
  }

  // Force the inherited set to be the same as the bounding set. If we
  // are root and capabilities have not been specified, then this is a
  // no-op. If capabilities have been specified, then we need to clip the
  // inherited set to prevent file-based capabilities granting privileges
  // outside the bounding set.
  capabilities->set(
      capabilities::INHERITABLE,
      capabilities->get(capabilities::BOUNDING));
}
#endif // __linux__


int MesosContainerizerLaunch::execute()
{
  if (flags.help) {
    cerr << flags.usage();
    return EXIT_SUCCESS;
  }

  // The existence of the `runtime_directory` flag implies that we
  // want to checkpoint the container's status upon exit.
  if (flags.runtime_directory.isSome()) {
    containerStatusPath = path::join(
        flags.runtime_directory.get(),
        containerizer::paths::STATUS_FILE);

    Try<int_fd> open = os::open(
        containerStatusPath.get(),
        O_WRONLY | O_CREAT | O_CLOEXEC,
        S_IRUSR | S_IWUSR);

    if (open.isError()) {
      cerr << "Failed to open file for writing the container status"
           << " '" << containerStatusPath.get() << "':"
           << " " << open.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    containerStatusFd = open.get();
  }

#ifndef __WINDOWS__
  // We need a signal fence here to ensure that `containerStatusFd` is
  // actually written to memory and not just to a temporary register.
  // Without this, it's possible that the signal handler we are about
  // to install would never see the correct value since there's no
  // guarantee that it is written to memory until this function
  // completes (which won't happen for a really long time because we
  // do a blocking `waitpid()` below).
  std::atomic_signal_fence(std::memory_order_relaxed);

  // Install signal handlers for all incoming signals.
  Try<Nothing> signals = installSignalHandlers();
  if (signals.isError()) {
    cerr << "Failed to install signal handlers: " << signals.error() << endl;
    exitWithStatus(EXIT_FAILURE);
  }
#else
  // We need a handle to the job object which this container is associated with.
  // Without this handle, the job object would be destroyed by the OS when the
  // agent exits (or crashes), making recovery impossible. By holding a handle,
  // we tie the lifetime of the job object to the container itself. In this way,
  // a recovering agent can reattach to the container by opening a new handle to
  // the job object.
  const pid_t pid = ::GetCurrentProcessId();
  const Try<std::wstring> name = os::name_job(pid);
  if (name.isError()) {
    cerr << "Failed to create job object name from pid: " << name.error()
         << endl;
    exitWithStatus(EXIT_FAILURE);
  }

  // NOTE: This handle will not be destructed, even though it is a
  // `SharedHandle`, because it will (purposefully) never go out of scope.
  Try<SharedHandle> handle = os::open_job(JOB_OBJECT_QUERY, false, name.get());
  if (handle.isError()) {
    cerr << "Failed to open job object '" << stringify(name.get())
         << "' for the current container: " << handle.error() << endl;
    exitWithStatus(EXIT_FAILURE);
  }
#endif // __WINDOWS__

  if (flags.launch_info.isNone()) {
    cerr << "Flag --launch_info is not specified" << endl;
    exitWithStatus(EXIT_FAILURE);
  }

  Try<ContainerLaunchInfo> _launchInfo =
    ::protobuf::parse<ContainerLaunchInfo>(flags.launch_info.get());

  if (_launchInfo.isError()) {
    cerr << "Failed to parse launch info: " << _launchInfo.error() << endl;
    exitWithStatus(EXIT_FAILURE);
  }

  ContainerLaunchInfo launchInfo = _launchInfo.get();

  if (!launchInfo.has_command()) {
    cerr << "Launch command is not specified" << endl;
    exitWithStatus(EXIT_FAILURE);
  }

  // Validate the command.
  if (launchInfo.command().shell()) {
    if (!launchInfo.command().has_value()) {
      cerr << "Shell command is not specified" << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  } else {
    if (!launchInfo.command().has_value()) {
      cerr << "Executable path is not specified" << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }

  if ((flags.pipe_read.isSome() && flags.pipe_write.isNone()) ||
      (flags.pipe_read.isNone() && flags.pipe_write.isSome())) {
    cerr << "Flag --pipe_read and --pipe_write should either be "
         << "both set or both not set" << endl;
    exitWithStatus(EXIT_FAILURE);
  }

  bool controlPipeSpecified =
    flags.pipe_read.isSome() && flags.pipe_write.isSome();

  if (controlPipeSpecified) {
    int_fd pipe[2] = { flags.pipe_read.get(), flags.pipe_write.get() };

    Try<Nothing> close = os::close(pipe[1]);
    if (close.isError()) {
      cerr << "Failed to close pipe[1]: " << close.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    // Do a blocking read on the pipe until the parent signals us to continue.
    char dummy;
    ssize_t length;
    while ((length = os::read(pipe[0], &dummy, sizeof(dummy))) == -1 &&
           errno == EINTR);

    if (length != sizeof(dummy)) {
      // There's a reasonable probability this will occur during
      // agent restarts across a large/busy cluster.
      cerr << "Failed to synchronize with agent "
           << "(it's probably exited)" << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    close = os::close(pipe[0]);
    if (close.isError()) {
      cerr << "Failed to close pipe[0]: " << close.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }

#ifndef __WINDOWS__
  if (launchInfo.has_tty_slave_path()) {
    Try<Nothing> setctty = os::setctty(STDIN_FILENO);
    if (setctty.isError()) {
      cerr << "Failed to set control tty: " << setctty.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }
#endif // __WINDOWS__

#ifdef __linux__
  // If we need a new mount namespace, we have to do it before
  // we make the mounts needed to prepare the rootfs template.
  if (flags.unshare_namespace_mnt) {
    if (unshare(CLONE_NEWNS) != 0) {
      cerr << "Failed to unshare mount namespace: "
           << os::strerror(errno) << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }

  if (flags.namespace_mnt_target.isSome()) {
    if (!launchInfo.mounts().empty()) {
      cerr << "Mounts are not supported if "
           << "'namespace_mnt_target' is set" << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    if (launchInfo.has_rootfs()) {
      cerr << "Container rootfs is not supported if "
           << "'namespace_mnt_target' is set" << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }
#endif // __linux__

  // Verify that the rootfs is an absolute path.
  if (launchInfo.has_rootfs()) {
    const string& rootfs = launchInfo.rootfs();

    cerr << "Preparing rootfs at '" << rootfs << "'" << endl;

    Result<string> realpath = os::realpath(rootfs);
    if (realpath.isError()) {
      cerr << "Failed to determine if rootfs '" << rootfs
           << "' is an absolute path: " << realpath.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    } else if (realpath.isNone()) {
      cerr << "Rootfs path '" << rootfs << "' does not exist" << endl;
      exitWithStatus(EXIT_FAILURE);
    } else if (realpath.get() != rootfs) {
      cerr << "Rootfs path '" << rootfs << "' is not an absolute path" << endl;
      exitWithStatus(EXIT_FAILURE);
    }

#ifdef __WINDOWS__
    cerr << "Changing rootfs is not supported on Windows";
    exitWithStatus(EXIT_FAILURE);
#endif // __WINDOWS__
  }

  Try<Nothing> mount = prepareMounts(launchInfo);
  if (mount.isError()) {
    cerr << "Failed to prepare mounts: " << mount.error() << endl;
    exitWithStatus(EXIT_FAILURE);
  }

  foreach (const string& target, launchInfo.masked_paths()) {
    mount = maskPath(target);
    if (mount.isError()) {
      cerr << "Failed to mask container paths: " << mount.error() << endl;
    }
  }

  foreach (const ContainerFileOperation& op, launchInfo.file_operations()) {
    Try<Nothing> result = executeFileOperation(op);
    if (result.isError()) {
      cerr << result.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }

  // Run additional preparation commands. These are run as the same
  // user and with the environment as the agent.
  foreach (const CommandInfo& command, launchInfo.pre_exec_commands()) {
    if (!command.has_value()) {
      cerr << "The 'value' of a preparation command is not specified" << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    cerr << "Executing pre-exec command '"
         << JSON::protobuf(command) << "'" << endl;

    Option<int> status;

    if (command.shell()) {
      // Execute the command using the system shell.
      status = os::system(command.value());
    } else {
      // Directly spawn all non-shell commands to prohibit users
      // from injecting arbitrary shell commands in the arguments.
      vector<string> args;
      foreach (const string& arg, command.arguments()) {
        args.push_back(arg);
      }

      status = os::spawn(command.value(), args);
    }

    if (status.isNone() || !WSUCCEEDED(status.get())) {
      cerr << "Failed to execute pre-exec command '"
           << JSON::protobuf(command) << "': ";
      if (status.isNone()) {
        cerr << "exited with unknown status" << endl;
      } else {
        cerr << WSTRINGIFY(status.get()) << endl;
      }
      exitWithStatus(EXIT_FAILURE);
    }
  }

#ifndef __WINDOWS__
  // NOTE: If 'user' is set, we will get the uid, gid, and the
  // supplementary group ids associated with the specified user before
  // changing the filesystem root. This is because after changing the
  // filesystem root, the current process might no longer have access
  // to /etc/passwd and /etc/group on the host.
  Option<uid_t> uid;
  Option<gid_t> gid;
  vector<gid_t> gids;

  // TODO(gilbert): For the case container user exists, support
  // framework/task/default user -> container user mapping once
  // user namespace and container capabilities is available for
  // mesos container.

  if (launchInfo.has_user()) {
    Result<uid_t> _uid = os::getuid(launchInfo.user());
    if (!_uid.isSome()) {
      cerr << "Failed to get the uid of user '" << launchInfo.user() << "': "
           << (_uid.isError() ? _uid.error() : "not found") << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    // No need to change user/groups if the specified user is the same
    // as the effective user of the current process.
    if (_uid.get() != ::geteuid()) {
      Result<gid_t> _gid = os::getgid(launchInfo.user());
      if (!_gid.isSome()) {
        cerr << "Failed to get the gid of user '" << launchInfo.user() << "': "
             << (_gid.isError() ? _gid.error() : "not found") << endl;
        exitWithStatus(EXIT_FAILURE);
      }

      Try<vector<gid_t>> _gids = os::getgrouplist(launchInfo.user());
      if (_gids.isError()) {
        cerr << "Failed to get the supplementary gids of user '"
             << launchInfo.user() << "': "
             << (_gids.isError() ? _gids.error() : "not found") << endl;
        exitWithStatus(EXIT_FAILURE);
      }

      foreach (uint32_t supplementaryGroup, launchInfo.supplementary_groups()) {
        _gids->push_back(supplementaryGroup);
      }

      uid = _uid.get();
      gid = _gid.get();
      gids = _gids.get();
    }
  }
#else
  if (launchInfo.has_user()) {
    cerr << "Switching user is not supported on Windows" << endl;
    exitWithStatus(EXIT_FAILURE);
  }
#endif // __WINDOWS__

#ifdef __linux__
  // Initialize capabilities support if necessary.
  Option<Capabilities> capabilitiesManager = None();
  const bool needSetCapabilities = launchInfo.has_effective_capabilities() ||
                                   launchInfo.has_bounding_capabilities();

  if (needSetCapabilities || launchInfo.has_seccomp_profile()) {
    Try<Capabilities> _capabilitiesManager = Capabilities::create();
    if (_capabilitiesManager.isError()) {
      cerr << "Failed to initialize capabilities support: "
           << _capabilitiesManager.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    capabilitiesManager = _capabilitiesManager.get();
  }

  // Prevent clearing of capabilities on `setuid`.
  if (needSetCapabilities && uid.isSome()) {
    Try<Nothing> keepCaps = capabilitiesManager->setKeepCaps();
    if (keepCaps.isError()) {
      cerr << "Failed to set process control for keeping capabilities "
           << "on potential uid change: " << keepCaps.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }
#else
  if (launchInfo.has_effective_capabilities() ||
      launchInfo.has_bounding_capabilities()) {
    cerr << "Capabilities are not supported on non Linux system" << endl;
    exitWithStatus(EXIT_FAILURE);
  }
#endif // __linux__

#ifdef __linux__
  if (flags.namespace_mnt_target.isSome()) {
    string path = path::join(
        "/proc",
        stringify(flags.namespace_mnt_target.get()),
        "ns",
        "mnt");

    Try<Nothing> setns = ns::setns(path, "mnt", false);
    if (setns.isError()) {
      cerr << "Failed to enter mount namespace: "
           << setns.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }
#endif // __linux__

  // Change root to a new root, if provided.
  if (launchInfo.has_rootfs()) {
    cerr << "Changing root to " << launchInfo.rootfs() << endl;

    Try<Nothing> enter = enterChroot(launchInfo.rootfs());

    if (enter.isError()) {
      cerr << enter.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }

  // Install resource limits for the process.
  if (launchInfo.has_rlimits()) {
    Try<Nothing> set = installResourceLimits(launchInfo.rlimits());

    if (set.isError()) {
      cerr << set.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }

  if (launchInfo.has_working_directory()) {
    // If working directory does not exist (e.g., being removed from
    // the container image), create an empty directory even it may
    // not be used. Please note that this case can only be possible
    // if an image has 'WORKDIR' specified in its manifest but that
    // 'WORKDIR' does not exist in the image's rootfs.
    //
    // TODO(gilbert): Set the proper ownership to this working
    // directory to make sure a specified non-root user has the
    // permission to write to this working directory. Right now
    // it is owned by root, and any non-root user will fail to
    // write to this directory. Please note that this is identical
    // to the semantic as docker daemon. The semantic can be
    // verified by:
    // 'docker run -ti -u nobody quay.io/spinnaker/front50:master bash'
    // The ownership of '/workdir' is root. Creating any file under
    // '/workdir' will fail for 'Permission denied'.
    Try<Nothing> mkdir = os::mkdir(launchInfo.working_directory());
    if (mkdir.isError()) {
      cerr << "Failed to create working directory "
           << "'" << launchInfo.working_directory() << "': "
           << mkdir.error() << endl;
    }

    Try<Nothing> chdir = os::chdir(launchInfo.working_directory());
    if (chdir.isError()) {
      cerr << "Failed to chdir into current working directory "
           << "'" << launchInfo.working_directory() << "': "
           << chdir.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }

#ifdef ENABLE_SECCOMP_ISOLATOR
  if (launchInfo.has_seccomp_profile()) {
    CHECK_SOME(capabilitiesManager);

    Try<ProcessCapabilities> capabilities = capabilitiesManager->get();
    if (capabilities.isError()) {
      cerr << "Failed to get capabilities for the current process: "
           << capabilities.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    calculateCapabilities(launchInfo, &capabilities.get());

    Try<Owned<SeccompFilter>> seccompFilter = SeccompFilter::create(
        launchInfo.seccomp_profile(),
        capabilities.get());

    if (seccompFilter.isError()) {
      cerr << "Failed to create Seccomp filter: "
           << seccompFilter.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    Try<Nothing> load = seccompFilter.get()->load();
    if (load.isError()) {
      cerr << "Failed to load Seccomp filter: " << load.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }
#endif // ENABLE_SECCOMP_ISOLATOR

#ifndef __WINDOWS__
  // Change user if provided. Note that we do that after executing the
  // preparation commands so that those commands will be run with the
  // same privilege as the mesos-agent.
  if (uid.isSome()) {
    Try<Nothing> setgid = os::setgid(gid.get());
    if (setgid.isError()) {
      cerr << "Failed to set gid to " << gid.get()
           << ": " << setgid.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    Try<Nothing> setgroups = os::setgroups(gids, uid);
    if (setgroups.isError()) {
      cerr << "Failed to set supplementary gids: "
           << setgroups.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    Try<Nothing> setuid = os::setuid(uid.get());
    if (setuid.isError()) {
      cerr << "Failed to set uid to " << uid.get()
           << ": " << setuid.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }
#endif // __WINDOWS__

#ifdef __linux__
  if (needSetCapabilities) {
    CHECK_SOME(capabilitiesManager);

    Try<ProcessCapabilities> capabilities = capabilitiesManager->get();
    if (capabilities.isError()) {
      cerr << "Failed to get capabilities for the current process: "
           << capabilities.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    // After 'setuid', the 'effective' set is cleared. Since `SETPCAP`
    // is required in the `effective` set of a process to change the
    // bounding set, we need to restore it first so we can make the
    // final capability changes.
    capabilities->add(capabilities::EFFECTIVE, capabilities::SETPCAP);

    Try<Nothing> setPcap = capabilitiesManager->set(capabilities.get());
    if (setPcap.isError()) {
      cerr << "Failed to add SETPCAP to the effective set: "
           << setPcap.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    calculateCapabilities(launchInfo, &capabilities.get());

    Try<Nothing> set = capabilitiesManager->set(capabilities.get());
    if (set.isError()) {
      cerr << "Failed to set process capabilities: " << set.error() << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }

  if (launchInfo.has_no_new_privileges()) {
    const int val = launchInfo.no_new_privileges() ? 1 : 0;
    if (prctl(PR_SET_NO_NEW_PRIVS, val, 0, 0, 0) == -1) {
      cerr << "Failed to set NO_NEW_PRIVS: " << os::strerror(errno) << endl;
      exitWithStatus(EXIT_FAILURE);
    }
  }
#endif // __linux__

  // Prepare the executable and the argument list for the child.
  string executable(launchInfo.command().shell()
    ? os::Shell::name
    : launchInfo.command().value().c_str());

  // Search executable in the current working directory as well.
  // execvpe and execvp will only search executable from the current
  // working directory if environment variable PATH is not set.
  if (!path::is_absolute(executable) &&
      launchInfo.has_working_directory()) {
    Option<string> which = os::which(
        executable,
        launchInfo.working_directory());

    if (which.isSome()) {
      executable = which.get();
    }
  }

  os::raw::Argv argv(launchInfo.command().shell()
    ? vector<string>({
          os::Shell::arg0,
          os::Shell::arg1,
          launchInfo.command().value()})
    : vector<string>(
          launchInfo.command().arguments().begin(),
          launchInfo.command().arguments().end()));

  // Prepare the environment for the child. If 'environment' is not
  // specified, inherit the environment of the current process.
  Option<os::raw::Envp> envp;
  if (launchInfo.has_environment()) {
    // TODO(tillt): `Environment::Variable` is not a string anymore,
    // consider cleaning this up with the complete rollout of `Secrets`.
    // This entire merging should be handled by the solution introduced
    // by MESOS-7299.
    hashmap<string, string> environment;

    foreach (const Environment::Variable& variable,
             launchInfo.environment().variables()) {
      const string& name = variable.name();
      const string& value = variable.value();

      // TODO(tillt): Once we have a solution for MESOS-7292, allow
      // logging of values.
      if (environment.contains(name) && environment[name] != value) {
        cerr << "Overwriting environment variable '" << name << "'" << endl;
      }

      environment[name] = value;
    }

    if (!environment.contains("PATH")) {
      environment["PATH"] = os::host_default_path();
    }

    envp = os::raw::Envp(environment);
  }

#ifndef __WINDOWS__
  // Construct a set of file descriptors to close before `exec`'ing.
  //
  // On Windows all new processes create by Mesos go through the
  // `create_process` wrapper which with the completion of MESOS-8926
  // will prevent inadvertent leaks making this code unnecessary there.
  Try<vector<int_fd>> fds = os::lsof();
  CHECK_SOME(fds);

  // If we have `containerStatusFd` set, then we need to fork-exec the
  // command we are launching and checkpoint its status on exit. We
  // use fork-exec directly (as opposed to `process::subprocess()`) to
  // avoid initializing libprocess for this simple helper binary.
  //
  // TODO(klueska): Once we move the majority of `process::subprocess()`
  // into stout, update the code below to use it.
  if (containerStatusFd.isSome()) {
    pid_t pid = ::fork();

    if (pid == -1) {
      cerr << "Failed to fork() the command: " << os::strerror(errno) << endl;
      exitWithStatus(EXIT_FAILURE);
    }

    // If we are the parent...
    if (pid > 0) {
      // Set the global `containerPid` variable to enable signal forwarding.
      //
      // NOTE: We need a signal fence here to ensure that `containerPid`
      // is actually written to memory and not just to a temporary register.
      // Without this, it's possible that the signal handler would
      // never notice the change since there's no guarantee that it is
      // written out to memory until this function completes (which
      // won't happen until it's too late because we loop inside a
      // blocking `waitpid()` call below).
      containerPid = pid;
      std::atomic_signal_fence(std::memory_order_relaxed);

      // Wait for the newly created process to finish.
      int status = 0;
      Result<pid_t> waitpid = None();

      // Reap all descendants, but only continue once we reap the
      // process we just launched.
      while (true) {
        waitpid = os::waitpid(-1, &status, 0);

        if (waitpid.isError()) {
          // If the error was an EINTR, we were interrupted by a
          // signal and should just call `waitpid()` over again.
          if (errno == EINTR) {
            continue;
          }
          cerr << "Failed to os::waitpid(): " << waitpid.error() << endl;
          exitWithStatus(EXIT_FAILURE);
        }

        if (waitpid.isNone()) {
          cerr << "Calling os::waitpid() with blocking semantics"
               << "returned asynchronously" << endl;
          exitWithStatus(EXIT_FAILURE);
        }

        // We only forward the signal if the child has terminated. If
        // the child has stopped due to some signal (e.g., SIGSTOP),
        // we will simply ignore it.
        if (WIFSTOPPED(status)) {
          continue;
        }

        if (pid == waitpid.get()) {
          break;
        }
      }

      signalSafeWriteStatus(status);
      os::close(containerStatusFd.get());
      ::_exit(EXIT_SUCCESS);
    }

    // Avoid leaking not required file descriptors into the forked process.
    foreach (int_fd fd, fds.get()) {
      if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        // NOTE: Set "FD_CLOEXEC" on the fd, instead of closing it
        // because exec below might exec a memfd.
        int flags = ::fcntl(fd, F_GETFD);
        if (flags == -1) {
          ABORT("Failed to get FD flags");
        }
        if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
          ABORT("Failed to set FD_CLOEXEC");
        }
      }
    }
  }
#endif // __WINDOWS__

  // NOTE: On Windows, these functions call `CreateProcess` and then wait for
  // the new process to exit. Because of this, the `SharedHandle` to the job
  // object does not go out of scope. This is unlike the POSIX behavior of
  // `exec`, as the process image is intentionally not replaced.
  if (envp.isSome()) {
    os::execvpe(executable.c_str(), argv, envp.get());
  } else {
    os::execvp(executable.c_str(), argv);
  }

  // If we get here, the execvp call failed.
  cerr << "Failed to execute '" << executable << "': "
       << os::strerror(errno) << endl;

  exitWithStatus(EXIT_FAILURE);
  UNREACHABLE();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
