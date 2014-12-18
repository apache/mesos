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

#include <string.h>
#include <unistd.h>

#include <iostream>

#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/unreachable.hpp>

#ifdef __linux__
#include "linux/fs.hpp"
#endif

#include "mesos/mesos.hpp"

#include "slave/containerizer/mesos/launch.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

const string MesosContainerizerLaunch::NAME = "launch";


MesosContainerizerLaunch::Flags::Flags()
{
  add(&command,
      "command",
      "The command to execute.");

  add(&directory,
      "directory",
      "The directory to chdir to. If rootfs is specified this must\n"
      "be relative to the new root.");

  add(&rootfs,
      "rootfs",
      "Absolute path to the container root filesystem.\n"
      "The command and directory flags are interpreted relative\n"
      "to rootfs\n"
      "Different platforms may implement 'chroot' differently.");

  add(&user,
      "user",
      "The user to change to.");

  add(&pipe_read,
      "pipe_read",
      "The read end of the control pipe.");

  add(&pipe_write,
      "pipe_write",
      "The write end of the control pipe.");

  add(&commands,
      "commands",
      "The additional preparation commands to execute before\n"
      "executing the command.");
}


int MesosContainerizerLaunch::execute()
{
  // Check command line flags.
  if (flags.command.isNone()) {
    cerr << "Flag --command is not specified" << endl;
    return 1;
  }

  if (flags.directory.isNone()) {
    cerr << "Flag --directory is not specified" << endl;
    return 1;
  }

  if (flags.pipe_read.isNone()) {
    cerr << "Flag --pipe_read is not specified" << endl;
    return 1;
  }

  if (flags.pipe_write.isNone()) {
    cerr << "Flag --pipe_write is not specified" << endl;
    return 1;
  }

  // Parse the command.
  Try<CommandInfo> command =
    ::protobuf::parse<CommandInfo>(flags.command.get());

  if (command.isError()) {
    cerr << "Failed to parse the command: " << command.error() << endl;
    return 1;
  }

  // Validate the command.
  if (command.get().shell()) {
    if (!command.get().has_value()) {
      cerr << "Shell command is not specified" << endl;
      return 1;
    }
  } else {
    if (!command.get().has_value()) {
      cerr << "Executable path is not specified" << endl;
      return 1;
    }
  }

  Try<Nothing> close = os::close(flags.pipe_write.get());
  if (close.isError()) {
    cerr << "Failed to close pipe[1]: " << close.error() << endl;
    return 1;
  }

  // Do a blocking read on the pipe until the parent signals us to continue.
  char dummy;
  ssize_t length;
  while ((length = ::read(
              flags.pipe_read.get(),
              &dummy,
              sizeof(dummy))) == -1 &&
          errno == EINTR);

  if (length != sizeof(dummy)) {
     // There's a reasonable probability this will occur during slave
     // restarts across a large/busy cluster.
     cerr << "Failed to synchronize with slave (it's probably exited)" << endl;
     return 1;
  }

  close = os::close(flags.pipe_read.get());
  if (close.isError()) {
    cerr << "Failed to close pipe[0]: " << close.error() << endl;
    return 1;
  }

  // Run additional preparation commands. These are run as the same
  // user and with the environment as the slave.
  if (flags.commands.isSome()) {
    // TODO(jieyu): Use JSON::Array if we have generic parse support.
    JSON::Object object = flags.commands.get();
    if (object.values.count("commands") == 0) {
      cerr << "Invalid JSON format for flag --commands" << endl;
      return 1;
    }

    if (!object.values["commands"].is<JSON::Array>()) {
      cerr << "Invalid JSON format for flag --commands" << endl;
      return 1;
    }

    JSON::Array array = object.values["commands"].as<JSON::Array>();
    foreach (const JSON::Value& value, array.values) {
      if (!value.is<JSON::Object>()) {
        cerr << "Invalid JSON format for flag --commands" << endl;
        return 1;
      }

      Try<CommandInfo> parse = ::protobuf::parse<CommandInfo>(value);
      if (parse.isError()) {
        cerr << "Failed to parse a preparation command: "
             << parse.error() << endl;
        return 1;
      }

      // TODO(jieyu): Currently, we only accept shell commands for the
      // preparation commands.
      if (!parse.get().shell()) {
        cerr << "Preparation commands need to be shell commands" << endl;
        return 1;
      }

      if (!parse.get().has_value()) {
        cerr << "The 'value' of a preparation command is not specified" << endl;
        return 1;
      }

      // Block until the command completes.
      int status = os::system(parse.get().value());
      if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
        cerr << "Failed to execute a preparation shell command" << endl;
        return 1;
      }
    }
  }

  // Change root to a new root, if provided.
  if (flags.rootfs.isSome()) {
    cout << "Changing root to " << flags.rootfs.get() << endl;

    // Verify that rootfs is an absolute path.
    Result<string> realpath = os::realpath(flags.rootfs.get());
    if (realpath.isError()) {
      cerr << "Failed to determine if rootfs is an absolute path: "
           << realpath.error() << endl;
      return 1;
    } else if (realpath.isNone()) {
      cerr << "Rootfs path does not exist" << endl;
      return 1;
    } else if (realpath.get() != flags.rootfs.get()) {
      cerr << "Rootfs path is not an absolute path" << endl;
      return 1;
    }

#ifdef __linux__
    Try<Nothing> chroot = fs::chroot::enter(flags.rootfs.get());
#else // For any other platform we'll just use POSIX chroot.
    Try<Nothing> chroot = os::chroot(flags.rootfs.get());
#endif // __linux__
    if (chroot.isError()) {
      cerr << "Failed to enter chroot '" << flags.rootfs.get()
           << "': " << chroot.error();
      return 1;
    }
  }

  // Change user if provided. Note that we do that after executing the
  // preparation commands so that those commands will be run with the
  // same privilege as the mesos-slave.
  // NOTE: The requisite user/group information must be present if
  // a container root filesystem is used.
  if (flags.user.isSome()) {
    Try<Nothing> su = os::su(flags.user.get());
    if (su.isError()) {
      cerr << "Failed to change user to '" << flags.user.get() << "': "
           << su.error() << endl;
      return 1;
    }
  }

  // Enter working directory, relative to the new root.
  Try<Nothing> chdir = os::chdir(flags.directory.get());
  if (chdir.isError()) {
    cerr << "Failed to chdir into work directory '"
         << flags.directory.get() << "': " << chdir.error() << endl;
    return 1;
  }

  // Relay the environment variables.
  // TODO(jieyu): Consider using a clean environment.

  if (command.get().shell()) {
    // Execute the command using shell.
    execl("/bin/sh", "sh", "-c", command.get().value().c_str(), (char*) NULL);
  } else {
    // Use os::execvpe to launch the command.
    char** argv = new char*[command.get().arguments().size() + 1];
    for (int i = 0; i < command.get().arguments().size(); i++) {
      argv[i] = strdup(command.get().arguments(i).c_str());
    }
    argv[command.get().arguments().size()] = NULL;

    execvp(command.get().value().c_str(), argv);
  }

  // If we get here, the execle call failed.
  cerr << "Failed to execute command" << endl;
  UNREACHABLE();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
