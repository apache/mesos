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

#include <process/internal.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include "launcher/launcher.hpp"

using namespace process;

using std::cerr;
using std::endl;
using std::map;
using std::string;

namespace mesos {
namespace internal {
namespace launcher {

// The default executable.
const string DEFAULT_EXECUTABLE = "mesos-launcher";


// The prefix of the environment variables that launcher uses.
static const string LAUNCHER_PREFIX = "MESOS_LAUNCHER_";


// The default directory to search for the executable.
static Option<string> defaultPath;
static int defaultPathLock = 0;


// Stores all the registered operations.
static hashmap<string, Owned<Operation> > operations;


static void usage(const char* argv0)
{
  cerr << "Usage: " << argv0 << " <operation> [OPTIONS]" << endl
       << endl
       << "Available operations:" << endl
       << "    help" << endl;

  // Get a list of available operations.
  foreachkey (const string& name, operations) {
    cerr << "    " << name << endl;
  }
}


void setDefaultPath(const string& path)
{
  process::internal::acquire(&defaultPathLock);
  {
    defaultPath = path;
  }
  process::internal::release(&defaultPathLock);
}


static Option<string> getDefaultPath()
{
  Option<string> path;

  process::internal::acquire(&defaultPathLock);
  {
    path = defaultPath;
  }
  process::internal::release(&defaultPathLock);

  return path;
}


void add(const Owned<Operation>& operation)
{
  operations[operation->name()] = operation;
}


int main(int argc, char** argv)
{
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  if (!strcmp(argv[1], "help")) {
    if (argc == 2) {
      usage(argv[0]);
      return 1;
    }

    // 'argv[0] help operation' => 'argv[0] operation --help'
    argv[1] = argv[2];
    argv[2] = (char*) "--help";
  }

  const string operation = argv[1];

  if (!operations.contains(operation)) {
    cerr << "Operation '" << operation << "' is not available" << endl;
    usage(argv[0]);
    return 1;
  }

  // Create the operation specific flags.
  flags::FlagsBase* flags = operations[operation]->getFlags();

  // Parse the flags from the environment and the command line.
  Try<Nothing> load = flags->load(LAUNCHER_PREFIX, argc, argv);
  if (load.isError()) {
    cerr << "Failed to parse the flags: " << load.error() << endl;
    return 1;
  }

  // Execute the operation.
  return operations[operation]->execute();
}


ShellOperation::Flags::Flags()
{
  add(&command,
      "command",
      "The shell command to be executed");
}


int ShellOperation::execute()
{
  if (flags.command.isNone()) {
    cerr << "The command is not specified" << endl;
    return 1;
  }

  int status = os::system(flags.command.get());
  if (!WIFEXITED(status)) {
    return 1;
  }

  return WEXITSTATUS(status);
}


process::Future<Option<int> > Operation::launch(
    const Option<int>& stdout,
    const Option<int>& stderr,
    const string& executable,
    const Option<string>& _path)
{
  // Determine the path to search for the executable. If the path is
  // specified by the user, use it. Otherwise, use the default path.
  // If both are not specified, return failure.
  string path;
  if (_path.isSome()) {
    path = _path.get();
  } else {
    Option<string> _defaultPath = getDefaultPath();
    if (_defaultPath.isNone()) {
      return Failure("Path is not specified and no default path is found");
    }
    path = _defaultPath.get();
  }

  Result<string> realpath = os::realpath(path::join(path, executable));
  if (!realpath.isSome()) {
    return Failure(
        "Failed to determine the canonical path for '" + executable + "': " +
        (realpath.isError() ? realpath.error() : "No such file or directory"));
  }

  // Prepare the environment variables.
  map<string, string> environment;
  foreachpair (const string& name, const flags::Flag& flag, *getFlags()) {
    Option<string> value = flag.stringify(*getFlags());
    if (value.isSome()) {
      string key = LAUNCHER_PREFIX + name;
      environment[key] = value.get();
      VLOG(1) << "Setting launcher environment " << key << "=" << value.get();
    }
  }

  // Prepare the command: 'mesos-launcher <operation_name> ...'.
  string command = strings::join(" ", realpath.get(), name());

  Try<Subprocess> s = subprocess(command, environment);
  if (s.isError()) {
    return Failure("Launch subprocess failed: " + s.error());
  }

  io::redirect(s.get().out(), stdout);
  io::redirect(s.get().err(), stderr);

  return s.get().status();
}

} // namespace launcher {
} // namespace internal {
} // namespace mesos {
