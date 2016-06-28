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

#include "launcher/posix/executor.hpp"

#include <iostream>

#include <stout/os.hpp>
#include <stout/strings.hpp>

#ifdef __linux__
#include "linux/fs.hpp"
#endif

#ifdef __linux__
namespace fs = mesos::internal::fs;
#endif

using std::cout;
using std::cerr;
using std::endl;
using std::string;
using std::vector;

namespace mesos {
namespace v1 {
namespace internal {

pid_t launchTaskPosix(
    const mesos::v1::TaskInfo& task,
    const mesos::v1::CommandInfo& command,
    const Option<string>& user,
    char** argv,
    Option<string>& rootfs,
    Option<string>& sandboxDirectory,
    Option<string>& workingDirectory)
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

  // Prepare the command log message.
  string commandString;

  if (command.shell()) {
    commandString = strings::format(
        "%s %s '%s'",
        os::Shell::arg0,
        os::Shell::arg1,
        command.value()).get();
  } else {
    commandString = strings::format(
        "[%s, %s]",
        command.value(),
        strings::join(", ", command.arguments())).get();
  }

  pid_t pid;
  if ((pid = fork()) == -1) {
    ABORT("Failed to fork to run '" + commandString + "'"
          ": " + os::strerror(errno));
  }

  // TODO(jieyu): Make the child process async signal safe.
  if (pid == 0) {
    // In child process, we make cleanup easier by putting process
    // into it's own session.
    // NOTE: POSIX guarantees a forked child's pid does not match any
    // existing process group id so only a single `setsid()` is
    // required and the session id will be the pid.
    if (::setsid() == -1) {
      ABORT("Failed to put child in a new session: " + os::strerror(errno));
    }

    if (rootfs.isSome()) {
      // NOTE: we need to put change user, chdir logics in command
      // executor because these depend on the new root filesystem.
      // If the command task does not change root fiesystem, these
      // will be handled in the containerizer.
#ifdef __linux__
      // NOTE: If 'user' is set, we will get the uid, gid, and the
      // supplementary group ids associated with the specified user
      // before changing the filesystem root. This is because after
      // changing the filesystem root, the current process might no
      // longer have access to /etc/passwd and /etc/group on the
      // host.
      Option<uid_t> uid;
      Option<gid_t> gid;
      vector<gid_t> gids;

      // TODO(gilbert): For the case container user exists, support
      // framework/task/default user -> container user mapping once
      // user namespace and container capabilities is available for
      // mesos container.

      if (user.isSome()) {
        Result<uid_t> _uid = os::getuid(user.get());
        if (!_uid.isSome()) {
          ABORT("Failed to get the uid of user '" + user.get() + "': " +
                (_uid.isError() ? _uid.error() : "not found"));
        }

        // No need to change user/groups if the specified user is
        // the same as that of the current process.
        if (_uid.get() != os::getuid().get()) {
          Result<gid_t> _gid = os::getgid(user.get());
          if (!_gid.isSome()) {
            ABORT("Failed to get the gid of user '" + user.get() + "': " +
                  (_gid.isError() ? _gid.error() : "not found"));
          }

          Try<vector<gid_t>> _gids = os::getgrouplist(user.get());
          if (_gids.isError()) {
            ABORT("Failed to get the supplementary gids of "
                  "user '" + user.get() + "': " +
                  (_gids.isError() ? _gids.error() : "not found"));
          }

          uid = _uid.get();
          gid = _gid.get();
          gids = _gids.get();
        }
      }

      Try<Nothing> chroot = fs::chroot::enter(rootfs.get());
      if (chroot.isError()) {
        ABORT("Failed to enter chroot '" + rootfs.get() + "'"
              ": " + chroot.error());
      }

      if (uid.isSome()) {
        Try<Nothing> setgid = os::setgid(gid.get());
        if (setgid.isError()) {
          ABORT("Failed to set gid to " + stringify(gid.get()) +
                ": " + setgid.error());
        }

        Try<Nothing> setgroups = os::setgroups(gids, uid);
        if (setgroups.isError()) {
          ABORT("Failed to set supplementary gids: " + setgroups.error());
        }

        Try<Nothing> setuid = os::setuid(uid.get());
        if (setuid.isError()) {
          ABORT("Failed to set uid to " + stringify(uid.get()) +
                ": " + setuid.error());
        }
      }

      // Determine the current working directory for the executor.
      string cwd;
      if (workingDirectory.isSome()) {
        cwd = workingDirectory.get();
      } else {
        CHECK_SOME(sandboxDirectory);
        cwd = sandboxDirectory.get();
      }

      Try<Nothing> chdir = os::chdir(cwd);
      if (chdir.isError()) {
        ABORT("Failed to chdir into current working directory "
              "'" + cwd + "': " + chdir.error());
      }
#else
      ABORT("Rootfs is only supported on Linux");
#endif // __linux__
    }

    cout << commandString << endl;

    // The child has successfully setsid, now run the command.
    if (command.shell()) {
      execlp(os::Shell::name,
             os::Shell::arg0,
             os::Shell::arg1,
             command.value().c_str(),
             (char*) nullptr);
    } else {
      execvp(command.value().c_str(), argv);
    }

    ABORT("Failed to exec: " + os::strerror(errno));
  }

  return pid;
}

} // namespace internal {
} // namespace v1 {
} // namespace mesos {
