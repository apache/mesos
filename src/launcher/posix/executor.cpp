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

#include <stout/os/raw/argv.hpp>

#include "internal/devolve.hpp"

#include "launcher/posix/executor.hpp"

#ifdef __linux__
#include "linux/fs.hpp"
#endif

#include "slave/containerizer/mesos/constants.hpp"
#include "slave/containerizer/mesos/launch.hpp"

#ifdef __linux__
namespace fs = mesos::internal::fs;
#endif

using process::SETSID;
using process::Subprocess;

using std::cout;
using std::cerr;
using std::endl;
using std::string;
using std::vector;

using mesos::internal::devolve;
using mesos::internal::slave::MESOS_CONTAINERIZER;
using mesos::internal::slave::MesosContainerizerLaunch;

namespace mesos {
namespace v1 {
namespace internal {

pid_t launchTaskPosix(
    const mesos::v1::CommandInfo& command,
    const string& launcherDir,
    const Option<string>& user,
    const Option<string>& rootfs,
    const Option<string>& sandboxDirectory,
    const Option<string>& workingDirectory)
{
  if (rootfs.isSome()) {
    // The command executor is responsible for chrooting into the
    // root filesystem and changing the user before exec-ing the
    // user process.
#ifdef __linux__
    Result<string> _user = os::user();
    if (_user.isError()) {
      ABORT("Failed to get current user: " + _user.error());
    } else if (_user.isNone()) {
      ABORT("Current username is not found");
    } else if (_user.get() != "root") {
      ABORT("The command executor requires root with rootfs");
    }
#else
    ABORT("Not expecting root volume with non-linux platform");
#endif // __linux__
  }

  // Prepare the flags to pass to the launch process.
  MesosContainerizerLaunch::Flags launchFlags;

  launchFlags.command = JSON::protobuf(devolve(command));

  if (rootfs.isSome()) {
    CHECK_SOME(sandboxDirectory);
    launchFlags.working_directory = workingDirectory.isSome()
      ? workingDirectory
      : sandboxDirectory;
  }

  launchFlags.rootfs = rootfs;
  launchFlags.user = user;

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
      SETSID,
      launchFlags);

  if (s.isError()) {
    ABORT("Failed to launch '" + commandString + "': " + s.error());
  }

  cout << commandString << endl;

  return s->pid();
}

} // namespace internal {
} // namespace v1 {
} // namespace mesos {
