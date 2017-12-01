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
// limitations under the License

#include <unistd.h>

#include <iostream>
#include <list>

#include <stout/fs.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/realpath.hpp>

using std::cerr;
using std::cout;
using std::endl;
using std::list;
using std::string;


void usage(const char* argv0)
{
  // Get a list of available commands.
  const Option<string> PATH = os::getenv("PATH");

  list<string> commands;

  if (PATH.isSome()) {
    foreach (const string& path, strings::split(PATH.get(), ":")) {
      Try<list<string>> matches = fs::list(path::join(path, "mesos-*"));
      if (matches.isSome()) {
        foreach (const string& match, matches.get()) {
          Try<bool> access = os::access(match, X_OK);
          if (access.isSome() && access.get()) {
            string basename = Path(match).basename();
            if (basename != "mesos-slave") {
              commands.push_back(basename.substr(6));
            }
          }
        }
      }
    }
  }

  cerr
    << "Usage: " << Path(argv0).basename() << " <command> [OPTIONS]"
    << endl
    << endl
    << "Available commands:" << endl
    << "    help" << endl;

  foreach (const string& command, commands) {
    cerr << "    " << command << endl;
  }
}


int main(int argc, char** argv)
{
  Option<string> value;

  // Try and add the absolute dirname of argv[0] to PATH so we can
  // find commands (since our installation directory might not be on
  // the path).
  Result<string> realpath = os::realpath(Path(argv[0]).dirname());
  if (realpath.isSome()) {
    value = os::getenv("PATH");
    if (value.isSome()) {
      os::setenv("PATH", realpath.get() + ":" + value.get());
    } else {
      os::setenv("PATH", realpath.get());
    }
  }

  if (argc < 2) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  // Update PYTHONPATH to include path to installed 'mesos' module.
  // TODO(benh): Remove this if/when we install the 'mesos' module via
  // PIP and setuptools.
  string path = path::join(PKGLIBEXECDIR, "python");
  value = os::getenv("PYTHONPATH");
  if (value.isSome()) {
    os::setenv("PYTHONPATH", value.get() + ":" + path);
  } else {
    os::setenv("PYTHONPATH", path);
  }

  // Now dispatch to any mesos-'command' on PATH.
  if (string(argv[1]) == "help") {
    if (argc == 2) {
      usage(argv[0]);
      return EXIT_SUCCESS;
    } else {
      // 'mesos help command' => 'mesos command --help'
      argv[1] = argv[2];
      argv[2] = (char*) "--help";
      return main(argc, argv);
    }
  } else if (string(argv[1]).find("--") == 0) {
    cerr << "Not expecting '" << argv[1] << "' before command" << endl;
    usage(argv[0]);
    return EXIT_FAILURE;
  } else {
    string command = argv[1];
    if (command == "slave") {
      cerr << "WARNING: subcommand 'slave' is deprecated in favor of 'agent'."
           << endl
           << endl;
    }
    string executable = "mesos-" + command;
    argv[1] = (char*) executable.c_str();
    execvp(executable.c_str(), argv + 1);
    if (errno == ENOENT) {
      cerr << "'" << command << "' is not a valid command "
           << "(or cannot be found)" << endl;
    } else {
      cerr << "Failed to execute '" << command << "': "
           << os::strerror(errno) << endl;
    }
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
