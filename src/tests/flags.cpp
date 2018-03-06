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

#include <mesos/type_utils.hpp>

#include <process/gtest.hpp>

#include <stout/check.hpp>
#include <stout/result.hpp>

#include <stout/os/realpath.hpp>

#include "master/constants.hpp"

#include "slave/constants.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using std::string;


namespace mesos {
namespace internal {
namespace tests {

// Storage for the flags.
Flags flags;

Flags::Flags()
{
  // We log to stderr by default, but when running tests we'd prefer
  // less junk to fly by, so force one to specify the verbosity.
  add(&Flags::verbose,
      "verbose",
      "Log all severity levels to stderr",
      false);

  add(&Flags::benchmark,
      "benchmark",
      "Run the benchmark tests (and skip other tests)",
      false);

  // We determine the defaults for 'source_dir' and 'build_dir' from
  // preprocessor definitions (at the time this comment was written
  // these were set via '-DSOURCE_DIR=...' and '-DBUILD_DIR=...' in
  // src/Makefile.am).
  Result<string> path = os::realpath(SOURCE_DIR);
  CHECK_SOME(path);
  add(&Flags::source_dir,
      "source_dir",
      "Where to find the source directory",
      path.get());

  path = os::realpath(BUILD_DIR);
  CHECK_SOME(path);

  add(&Flags::build_dir,
      "build_dir",
      "Where to find the build directory",
      path.get());

  add(&Flags::docker,
      "docker",
      "Where to find docker executable",
      "docker");

  add(&Flags::docker_socket,
      "docker_socket",
      "Resource used by the agent and the executor to provide CLI access\n"
      "to the Docker daemon. On Unix, this is typically a path to a\n"
      "socket, such as '/var/run/docker.sock'. On Windows this must be a\n"
      "named pipe, such as '//./pipe/docker_engine'. NOTE: This must be\n"
      "the path used by the Docker image used to run the agent.\n",
      slave::DEFAULT_DOCKER_HOST_RESOURCE);

  // This help message for --modules flag is the same for
  // {master,slave,sched,tests}/flags.[ch]pp and should always be kept in
  // sync.
  // TODO(karya): Remove the JSON example and add reference to the
  // doc file explaining the --modules flag.
  add(&Flags::modules,
      "modules",
      "List of modules to be loaded and be available to the internal\n"
      "subsystems.\n"
      "\n"
      "Use --modules=filepath to specify the list of modules via a\n"
      "file containing a JSON-formatted string. 'filepath' can be\n"
      "of the form 'file:///path/to/file' or '/path/to/file'.\n"
      "\n"
      "Use --modules=\"{...}\" to specify the list of modules inline.\n"
      "\n"
      "Example:\n"
      "{\n"
      "  \"libraries\": [\n"
      "    {\n"
      "      \"file\": \"/path/to/libfoo.so\",\n"
      "      \"modules\": [\n"
      "        {\n"
      "          \"name\": \"org_apache_mesos_bar\",\n"
      "          \"parameters\": [\n"
      "            {\n"
      "              \"key\": \"X\",\n"
      "              \"value\": \"Y\"\n"
      "            }\n"
      "          ]\n"
      "        },\n"
      "        {\n"
      "          \"name\": \"org_apache_mesos_baz\"\n"
      "        }\n"
      "      ]\n"
      "    },\n"
      "    {\n"
      "      \"name\": \"qux\",\n"
      "      \"modules\": [\n"
      "        {\n"
      "          \"name\": \"org_apache_mesos_norf\"\n"
      "        }\n"
      "      ]\n"
      "    }\n"
      "  ]\n"
      "}");

  // This help message for --modules_dir flag is the same for
  // {master,slave,sched,tests}/flags.[ch]pp and should always be kept in
  // sync.
  add(&Flags::modulesDir,
      "modules_dir",
      "Directory path of the module manifest files.\n"
      "The manifest files are processed in alphabetical order.\n"
      "(See --modules for more information on module manifest files).\n"
      "Cannot be used in conjunction with --modules.\n");

  // This help message is duplicated from slave/flags.hpp and
  // should always be kept in sync with that.
  add(&Flags::isolation,
      "isolation",
      "Isolation mechanisms to use, e.g., 'posix/cpu,posix/mem', or\n"
      "'cgroups/cpu,cgroups/mem', or network/port_mapping\n"
      "(configure with flag: --with-network-isolator to enable),\n"
      "or 'external', or load an alternate isolator module using\n"
      "the --modules flag.");

  // This help message is duplicated from master/flags.hpp and
  // should always be kept in sync with that.
  add(&Flags::authenticators,
      "authenticators",
      "Authenticator implementation to use when authenticating frameworks\n"
      "and/or agents. Use the default '" +
      string(master::DEFAULT_AUTHENTICATOR) + "', or\n"
      "load an alternate authenticator module using --modules.",
      master::DEFAULT_AUTHENTICATOR);

  // NOTE: The default await timeout matches process::TEST_AWAIT_TIMEOUT,
  // but we can't use that value directly due to static initializer ordering.
  add(&Flags::test_await_timeout,
      "test_await_timeout",
      "The default timeout for awaiting test events.",
      Seconds(15));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
