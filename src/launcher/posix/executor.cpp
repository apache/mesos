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

#include <iostream>

#include <process/subprocess.hpp>

#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include <mesos/slave/containerizer.hpp>

#include "launcher/posix/executor.hpp"

#ifdef __linux__
#include "linux/fs.hpp"
#endif

#include "slave/containerizer/mesos/constants.hpp"
#include "slave/containerizer/mesos/launch.hpp"

#ifdef __linux__
namespace fs = mesos::internal::fs;
#endif

using process::Subprocess;

using std::cout;
using std::cerr;
using std::endl;
using std::string;
using std::vector;

using mesos::internal::slave::MESOS_CONTAINERIZER;
using mesos::internal::slave::MesosContainerizerLaunch;

using mesos::slave::ContainerLaunchInfo;

namespace mesos {
namespace internal {

pid_t launchTaskPosix(
    const CommandInfo& command,
    const string& launcherDir,
    const Option<string>& user,
    const Option<string>& rootfs,
    const Option<string>& sandboxDirectory,
    const Option<string>& workingDirectory,
    const Option<CapabilityInfo>& capabilities)
{
  // Prepare the flags to pass to the launch process.
  MesosContainerizerLaunch::Flags launchFlags;

  ContainerLaunchInfo launchInfo;
  launchInfo.mutable_command()->CopyFrom(command);

  if (rootfs.isSome()) {
    // The command executor is responsible for chrooting into the
    // root filesystem and changing the user before exec-ing the
    // user process.
#ifdef __linux__
    if (geteuid() != 0) {
      ABORT("The command executor requires root with rootfs");
    }

    // Ensure that mount namespace of the executor is not affected by
    // changes in its task's namespace induced by calling `pivot_root`
    // as part of the task setup in mesos-containerizer binary.
    launchFlags.unshare_namespace_mnt = true;
#else
    ABORT("Not expecting root volume with non-linux platform");
#endif // __linux__

    launchInfo.set_rootfs(rootfs.get());

    CHECK_SOME(sandboxDirectory);

    launchInfo.set_working_directory(workingDirectory.isSome()
      ? workingDirectory.get()
      : sandboxDirectory.get());

    // TODO(jieyu): If the task has a rootfs, the executor itself will
    // be running as root. Its sandbox is owned by root as well. In
    // order for the task to be able to access to its sandbox, we need
    // to make sure the owner of the sandbox is 'user'. However, this
    // is still a workaround. The owner of the files downloaded by the
    // fetcher is still not correct (i.e., root).
    if (user.isSome()) {
      // NOTE: We only chown the sandbox directory (non-recursively).
      Try<Nothing> chown = os::chown(user.get(), os::getcwd(), false);
      if (chown.isError()) {
        ABORT("Failed to chown sandbox to user " +
              user.get() + ": " + chown.error());
      }
    }
  }

  if (user.isSome()) {
    launchInfo.set_user(user.get());
  }

  if (capabilities.isSome()) {
    launchInfo.mutable_capabilities()->CopyFrom(capabilities.get());
  }

  launchFlags.launch_info = JSON::protobuf(launchInfo);

  string commandString = strings::format(
      "%s %s %s",
      path::join(launcherDir, MESOS_CONTAINERIZER),
      MesosContainerizerLaunch::NAME,
      stringify(launchFlags)).get();

  // Fork the child using launcher.
  vector<string> argv(2);
  argv[0] = MESOS_CONTAINERIZER;
  argv[1] = MesosContainerizerLaunch::NAME;

  Try<Subprocess> s = subprocess(
      path::join(launcherDir, MESOS_CONTAINERIZER),
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      &launchFlags,
      None(),
      None(),
      {},
      {Subprocess::ChildHook::SETSID()});

  if (s.isError()) {
    ABORT("Failed to launch '" + commandString + "': " + s.error());
  }

  cout << commandString << endl;

  return s->pid();
}

} // namespace internal {
} // namespace mesos {
