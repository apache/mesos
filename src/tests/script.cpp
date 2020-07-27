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

#include <signal.h> // For strsignal.
#include <stdio.h>  // For freopen.

#include <sys/wait.h> // For wait (and associated macros).

#include <string>

#include <stout/check.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/uri.hpp>

#include <stout/os/realpath.hpp>

#include <stout/os/constants.hpp>

#include "common/status_utils.hpp"

#include "mesos/mesos.hpp"

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/script.hpp"
#include "tests/utils.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace tests {

void execute(const string& script)
{
  // Create a temporary directory for the test.
  Try<string> directory = environment->mkdtemp();

  CHECK_SOME(directory) << "Failed to create temporary directory";

  if (flags.verbose) {
    std::cerr << "Using temporary directory '"
              << directory.get() << "'" << std::endl;
  }

  // Determine the path for the script.
  Result<string> path = os::realpath(getTestScriptPath(script));

  if (!path.isSome()) {
    FAIL() << "Failed to locate script "
           << script << ": "
           << (path.isError() ? path.error() : "No such file or directory");
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
    CHECK(WIFEXITED(status) || WIFSIGNALED(status))
      << "Unexpected wait status " << status;

    if (!WSUCCEEDED(status)) {
      FAIL() << script << " " << WSTRINGIFY(status);
    }
  } else {
    // In child process. DO NOT USE GLOG!

    // Start by cd'ing into the temporary directory.
    Try<Nothing> chdir = os::chdir(directory.get());
    if (chdir.isError()) {
      std::cerr << "Failed to chdir to '" << directory.get() << "': "
                << chdir.error() << std::endl;
      abort();
    }

    // Redirect output to /dev/null unless the test is verbose.
    if (!flags.verbose) {
      if (freopen(os::DEV_NULL, "w", stdout) == nullptr ||
          freopen(os::DEV_NULL, "w", stderr) == nullptr) {
        std::cerr << "Failed to redirect stdout/stderr to /dev/null:"
                  << os::strerror(errno) << std::endl;
        abort();
      }
    }

    // Set up the environment for executing the script. We might be running from
    // the Mesos source tree or from an installed version of the tests. In the
    // latter case, all of the variables below are swizzled to point to the
    // installed locations, except MESOS_SOURCE_DIR. Scripts that make use of
    // MESOS_SOURCE_DIR are expected to gracefully degrade if the Mesos source
    // is no longer present.
    os::setenv("MESOS_BUILD_DIR", flags.build_dir);
    os::setenv("MESOS_HELPER_DIR", getTestHelperDir());
    os::setenv("MESOS_LAUNCHER_DIR", getLauncherDir());
    os::setenv("MESOS_SBIN_DIR", getSbinDir());
    os::setenv("MESOS_SOURCE_DIR", flags.source_dir);
    os::setenv("MESOS_WEBUI_DIR", getWebUIDir());

    // Enable replicated log based registry.
    os::setenv("MESOS_REGISTRY", "replicated_log");

    // Create test credentials.
    JSON::Object credential;
    credential.values["principal"] = DEFAULT_CREDENTIAL.principal();
    credential.values["secret"] = DEFAULT_CREDENTIAL.secret();

    JSON::Array array;
    array.values.push_back(credential);

    JSON::Object credentials;
    credentials.values["credentials"] = array;

    const string& credentialsPath =
      path::join(directory.get(), "credentials");

    CHECK_SOME(os::write(credentialsPath, stringify(credentials)))
      << "Failed to write credentials to '" << credentialsPath << "'";

    os::setenv("MESOS_CREDENTIALS", uri::from_path(credentialsPath));

    // Enable framework authentication on the master.
    os::setenv("MESOS_AUTHENTICATE_FRAMEWORKS", "true");

    // Enable authentication on the test framework.
    os::setenv("MESOS_EXAMPLE_AUTHENTICATE", "true");

    // We set test credentials here for example frameworks to use.
    os::setenv("MESOS_EXAMPLE_PRINCIPAL", DEFAULT_CREDENTIAL.principal());
    os::setenv("MESOS_EXAMPLE_SECRET", DEFAULT_CREDENTIAL.secret());

    // Create test ACLs.
    ACLs acls;
    acls.set_permissive(false);

    mesos::ACL::RunTask* run = acls.add_run_tasks();
    run->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());

    Result<string> user = os::user();
    CHECK_SOME(user) << "Failed to get current user name";
    run->mutable_users()->add_values(user.get());

    mesos::ACL::RegisterFramework* register_ = acls.add_register_frameworks();
    register_->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    register_->mutable_roles()->set_type(mesos::ACL::Entity::ANY);

    // Allow agents with any principal or no principal to register.
    // Currently the agents in the example tests don't have authentication
    // enabled so the agent's principal would be none.
    // TODO(xujyan): Enable agent authN and authZ by default in example tests.
    mesos::ACL::RegisterAgent* registerAgent = acls.add_register_agents();
    registerAgent->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    registerAgent->mutable_agents()->set_type(mesos::ACL::Entity::ANY);

    const string& aclsPath = path::join(directory.get(), "acls");

    CHECK_SOME(os::write(aclsPath, stringify(JSON::protobuf(acls))))
      << "Failed to write ACLs to '" << aclsPath << "'";

    os::setenv("MESOS_ACLS", uri::from_path(aclsPath));

    // Now execute the script.
    execl(path->c_str(), path->c_str(), (char*) nullptr);

    std::cerr << "Failed to execute '" << script << "': "
              << os::strerror(errno) << std::endl;
    abort();
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
