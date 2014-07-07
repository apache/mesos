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

#include <unistd.h>

#include <iostream>
#include <map>

#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/execenv.hpp>

#include "mesos/mesos.hpp"

#include "slave/containerizer/mesos/launch.hpp"

using std::cerr;
using std::endl;
using std::map;
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
      "The directory to chdir to.");

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

      // Block until the command completes.
      int status = os::system(parse.get().value());
      if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
        cerr << "Failed to execute a preparation command" << endl;
        return 1;
      }
    }
  }

  // Enter working directory.
  if (!os::chdir(flags.directory.get())) {
    cerr << "Failed to chdir into work directory "
         << flags.directory.get() << endl;
    return 1;
  }

  // Change user if provided. Note that we do that after executing the
  // preparation commands so that those commands will be run with the
  // same privilege as the mesos-slave.
  if (flags.user.isSome() && !os::su(flags.user.get())) {
    cerr << "Failed to change user to " << flags.user.get() << endl;
    return 1;
  }

  // Relay the environment variables.
  // TODO(jieyu): Consider using a clean environment.
  map<string, string> env;
  os::ExecEnv envp(env);

  // Execute the command (via '/bin/sh -c command') with its environment.
  execle(
      "/bin/sh",
      "sh",
      "-c",
      command.get().value().c_str(),
      (char*) NULL,
      envp());

  // If we get here, the execle call failed.
  cerr << "Failed to execute command" << endl;

  return UNREACHABLE();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
