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
    Option<char**>& override,
    Option<string>& rootfs,
    Option<string>& sandboxDirectory,
    Option<string>& workingDirectory)
{
  // TODO(benh): Clean this up with the new 'Fork' abstraction.
  // Use pipes to determine which child has successfully changed
  // session. This is needed as the setsid call can fail from other
  // processes having the same group id.
  int _pipes[2];

  Try<Nothing> pipes = os::pipe(_pipes);
  if (pipes.isError())
  {
    cerr << "Failed to create pipe: " << pipes.error() << endl;
    abort();
  }
  // Set the FD_CLOEXEC flags on these pipes.
  Try<Nothing> cloexec = os::cloexec(_pipes[0]);
  if (cloexec.isError()) {
    cerr << "Failed to cloexec(pipe[0]): " << cloexec.error() << endl;
    abort();
  }

  cloexec = os::cloexec(_pipes[1]);
  if (cloexec.isError()) {
    cerr << "Failed to cloexec(pipe[1]): " << cloexec.error() << endl;
    abort();
  }

  if (rootfs.isSome()) {
    // The command executor is responsible for chrooting into the
    // root filesystem and changing the user before exec-ing the
    // user process.
#ifdef __linux__
    Result<string> user = os::user();
    if (user.isError()) {
      cerr << "Failed to get current user: " << user.error() << endl;
      abort();
    } else if (user.isNone()) {
      cerr << "Current username is not found" << endl;
      abort();
    } else if (user.get() != "root") {
      cerr << "The command executor requires root with rootfs" << endl;
      abort();
    }
#else
    cerr << "Not expecting root volume with non-linux platform." << endl;
    abort();
#endif // __linux__
  }

  // Prepare the command log message.
  string commandString;
  if (override.isSome()) {
    char** argv = override.get();
    // argv is guaranteed to be nullptr terminated and we rely on
    // that fact to print command to be executed.
    for (int i = 0; argv[i] != nullptr; i++) {
      commandString += string(argv[i]) + " ";
    }
  } else if (command.shell()) {
    commandString = string(os::Shell::arg0) + " " +
      string(os::Shell::arg1) + " '" +
      command.value() + "'";
  } else {
    commandString =
      "[" + command.value() + ", " +
      strings::join(", ", command.arguments()) + "]";
  }

  pid_t pid;
  if ((pid = fork()) == -1) {
    cerr << "Failed to fork to run " << commandString << ": "
         << os::strerror(errno) << endl;
    abort();
  }

  // TODO(jieyu): Make the child process async signal safe.
  if (pid == 0) {
    // In child process, we make cleanup easier by putting process
    // into it's own session.
    os::close(_pipes[0]);

    // NOTE: We setsid() in a loop because setsid() might fail if another
    // process has the same process group id as the calling process.
    while ((pid = setsid()) == -1) {
      perror("Could not put command in its own session, setsid");

      cout << "Forking another process and retrying" << endl;

      if ((pid = fork()) == -1) {
        perror("Failed to fork to launch command");
        abort();
      }

      if (pid > 0) {
        // In parent process. It is ok to suicide here, because
        // we're not watching this process.
        exit(0);
      }
    }

    if (write(_pipes[1], &pid, sizeof(pid)) != sizeof(pid)) {
      perror("Failed to write PID on pipe");
      abort();
    }

    os::close(_pipes[1]);

    if (rootfs.isSome()) {
#ifdef __linux__
      if (user.isSome()) {
        // This is a work around to fix the problem that after we chroot
        // os::su call afterwards failed because the linker may not be
        // able to find the necessary library in the rootfs.
        // We call os::su before chroot here to force the linker to load
        // into memory.
        // We also assume it's safe to su to "root" user since
        // filesystem/linux.cpp checks for root already.
        os::su("root");
      }

      Try<Nothing> chroot = fs::chroot::enter(rootfs.get());
      if (chroot.isError()) {
        cerr << "Failed to enter chroot '" << rootfs.get()
             << "': " << chroot.error() << endl;
        abort();
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
        cerr << "Failed to chdir into current working directory '"
             << cwd << "': " << chdir.error() << endl;
        abort();
      }

      if (user.isSome()) {
        Try<Nothing> su = os::su(user.get());
        if (su.isError()) {
          cerr << "Failed to change user to '" << user.get() << "': "
               << su.error() << endl;
          abort();
        }
      }
#else
      cerr << "Rootfs is only supported on Linux" << endl;
      abort();
#endif // __linux__
    }

    cout << commandString << endl;

    // The child has successfully setsid, now run the command.
    if (override.isNone()) {
      if (command.shell()) {
        execlp(
                os::Shell::name,
                os::Shell::arg0,
                os::Shell::arg1,
                command.value().c_str(),
                (char*) nullptr);
      } else {
        execvp(command.value().c_str(), argv);
      }
    } else {
      char** argv = override.get();
      execvp(argv[0], argv);
    }

    perror("Failed to exec");
    abort();
  }

  // In parent process.
  os::close(_pipes[1]);

  // Get the child's pid via the pipe.
  if (read(_pipes[0], &pid, sizeof(pid)) == -1) {
    cerr << "Failed to get child PID from pipe, read: "
         << os::strerror(errno) << endl;
    abort();
  }

  os::close(_pipes[0]);

  return pid;
}

} // namespace internal {
} // namespace v1 {
} // namespace mesos {
