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

#include <signal.h> // For strsignal.
#include <stdio.h>  // For freopen.
#include <string.h> // For strlen, strerror.
#include <unistd.h> // For sysconf.

#include <sys/wait.h> // For wait (and associated macros).

#include <string>
#include <vector>

#include <stout/check.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/script.hpp"

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

void execute(const string& script, const vector<string>& arguments)
{
  static long maximumArguments = sysconf(_SC_ARG_MAX);
  PCHECK(maximumArguments != -1);

  CHECK_LT(arguments.size(), static_cast<size_t>(maximumArguments));

  // Create a temporary directory for the test.
  Try<string> directory = environment->mkdtemp();

  CHECK_SOME(directory) << "Failed to create temporary directory";

  if (flags.verbose) {
    std::cerr << "Using temporary directory '"
              << directory.get() << "'" << std::endl;
  }

  // Determine the path for the script.
  Try<string> path =
    os::realpath(path::join(flags.source_dir, "src", "tests", script));

  if (path.isError()) {
    FAIL() << "Failed to locate script: " << path.error();
  }

  // Fork a process to change directory and run the test.
  pid_t pid;
  if ((pid = fork()) == -1) {
    FAIL() << "Failed to fork to launch script";
  }

  if (pid > 0) {
    // In parent process.
    int status;
    while (wait(&status) != pid || WIFSTOPPED(status));
    CHECK(WIFEXITED(status) || WIFSIGNALED(status));

    if (WIFEXITED(status)) {
      if (WEXITSTATUS(status) != 0) {
        FAIL() << script << " exited with status " << WEXITSTATUS(status);
      }
    } else {
      FAIL() << script << " terminated with signal '"
             << strsignal(WTERMSIG(status)) << "'";
    }
  } else {
    // In child process. DO NOT USE GLOG!

    // Start by cd'ing into the temporary directory.
    if (!os::chdir(directory.get())) {
      std::cerr << "Failed to chdir to '" << directory.get() << "'" << std::endl;
      abort();
    }

    // Redirect output to /dev/null unless the test is verbose.
    if (!flags.verbose) {
      if (freopen("/dev/null", "w", stdout) == NULL ||
          freopen("/dev/null", "w", stderr) == NULL) {
        std::cerr << "Failed to redirect stdout/stderr to /dev/null:"
                  << strerror(errno) << std::endl;
        abort();
      }
    }

    // Set up the environment for executing the script.
    os::setenv("MESOS_SOURCE_DIR", flags.source_dir);
    os::setenv("MESOS_BUILD_DIR", flags.build_dir);
    os::setenv("MESOS_WEBUI_DIR", path::join(flags.source_dir, "src", "webui"));
    os::setenv("MESOS_LAUNCHER_DIR", path::join(flags.build_dir, "src"));

    // Enable authentication.
    os::setenv("MESOS_AUTHENTICATE", "true");

    // Create test credentials.
    const std::string& credentialsPath =
      path::join(directory.get(), "credentials");

    const std::string& credentials =
      DEFAULT_CREDENTIAL.principal() + " " + DEFAULT_CREDENTIAL.secret();

    CHECK_SOME(os::write(credentialsPath, credentials))
      << "Failed to write credentials to '" << credentialsPath << "'";

    os::setenv("MESOS_CREDENTIALS", "file://" + credentialsPath);

    // We set test credentials here for example frameworks to use.
    os::setenv("DEFAULT_PRINCIPAL", DEFAULT_CREDENTIAL.principal());
    os::setenv("DEFAULT_SECRET", DEFAULT_CREDENTIAL.secret());

    // Construct the argument array.
    const char** args = (const char**) new char*[arguments.size() + 1];
    args[0] = path.get().c_str();
    size_t index = 1;
    foreach (const string& argument, arguments) {
      args[index++] = argument.c_str();
    }
    args[arguments.size() + 1] = NULL;

    // Now execute the script.
    execv(path.get().c_str(), (char* const*) args);

    std::cerr << "Failed to execute '" << script << "': "
              << strerror(errno) << std::endl;
    abort();
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
