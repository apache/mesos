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

#include <stdio.h>

#include <map>
#include <string>

#include <mesos/executor.hpp>
#include <mesos/mesos.hpp>

#include <process/collect.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/loop.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include <stout/error.hpp>
#include <stout/flags.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>
#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <stout/os/killtree.hpp>

#ifdef __WINDOWS__
#include <stout/os/windows/jobobject.hpp>
#endif // __WINDOWS__

#include "checks/checks_runtime.hpp"
#include "checks/health_checker.hpp"

#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"

#include "docker/docker.hpp"
#include "docker/executor.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "messages/flags.hpp"
#include "messages/messages.hpp"

#include "slave/constants.hpp"

using namespace mesos;
using namespace process;

using std::cerr;
using std::cout;
using std::endl;
using std::map;
using std::string;
using std::vector;


int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

#ifdef __WINDOWS__
  // We need a handle to the job object which this container is associated with.
  // Without this handle, the job object would be destroyed by the OS when the
  // agent exits (or crashes), making recovery impossible. By holding a handle,
  // we tie the lifetime of the job object to the container itself. In this way,
  // a recovering agent can reattach to the container by opening a new handle to
  // the job object.
  const pid_t pid = ::GetCurrentProcessId();
  const Try<std::wstring> name = os::name_job(pid);
  if (name.isError()) {
    cerr << "Failed to create job object name from pid: " << name.error()
         << endl;
    return EXIT_FAILURE;
  }

  // NOTE: This handle will not be destructed, even though it is a
  // `SharedHandle`, because it will (purposefully) never go out of scope.
  Try<SharedHandle> handle = os::open_job(JOB_OBJECT_QUERY, false, name.get());
  if (handle.isError()) {
    cerr << "Failed to open job object '" << stringify(name.get())
         << "' for the current container: " << handle.error() << endl;
    return EXIT_FAILURE;
  }
#endif // __WINDOWS__

  mesos::internal::docker::Flags flags;

  // Load flags from environment and command line.
  Try<flags::Warnings> load = flags.load(None(), &argc, &argv);

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  mesos::internal::logging::initialize(argv[0], true, flags); // Catch signals.

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  VLOG(1) << stringify(flags);

  if (flags.docker.isNone()) {
    EXIT(EXIT_FAILURE) << flags.usage("Missing required option --docker");
  }

  if (flags.container.isNone()) {
    EXIT(EXIT_FAILURE) << flags.usage("Missing required option --container");
  }

  if (flags.sandbox_directory.isNone()) {
    EXIT(EXIT_FAILURE)
      << flags.usage("Missing required option --sandbox_directory");
  }

  if (flags.mapped_directory.isNone()) {
    EXIT(EXIT_FAILURE)
      << flags.usage("Missing required option --mapped_directory");
  }

  map<string, string> taskEnvironment;
  if (flags.task_environment.isSome()) {
    // Parse the string as JSON.
    Try<JSON::Object> json =
      JSON::parse<JSON::Object>(flags.task_environment.get());

    if (json.isError()) {
      EXIT(EXIT_FAILURE)
        << flags.usage("Failed to parse --task_environment: " + json.error());
    }

    // Convert from JSON to map.
    foreachpair (
        const string& key,
        const JSON::Value& value,
        json->values) {
      if (!value.is<JSON::String>()) {
        EXIT(EXIT_FAILURE) << flags.usage(
            "Value of key '" + key + "' in --task_environment is not a string");
      }

      // Save the parsed and validated key/value.
      taskEnvironment[key] = value.as<JSON::String>().value;
    }
  }

  Option<mesos::internal::ContainerDNSInfo> defaultContainerDNS;
  if (flags.default_container_dns.isSome()) {
    Try<mesos::internal::ContainerDNSInfo> parse =
      flags::parse<mesos::internal::ContainerDNSInfo>(
          flags.default_container_dns.get());

    if (parse.isError()) {
      EXIT(EXIT_FAILURE) << flags.usage(
          "Failed to parse --default_container_dns: " + parse.error());
    }

    defaultContainerDNS = parse.get();
  }

  // Get executor shutdown grace period from the environment.
  //
  // NOTE: We avoided introducing a docker executor flag for this
  // because the docker executor exits if it sees an unknown flag.
  // This makes it difficult to add or remove docker executor flags
  // that are unconditionally set by the agent.
  Duration shutdownGracePeriod =
    mesos::internal::slave::DEFAULT_EXECUTOR_SHUTDOWN_GRACE_PERIOD;
  Option<string> value = os::getenv("MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD");
  if (value.isSome()) {
    Try<Duration> parse = Duration::parse(value.get());
    if (parse.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to parse value '" << value.get() << "'"
        << " of 'MESOS_EXECUTOR_SHUTDOWN_GRACE_PERIOD': " << parse.error();
    }

    shutdownGracePeriod = parse.get();
  }

  // If the deprecated flag is set, respect it and choose the bigger value.
  //
  // TODO(alexr): Remove this after the deprecation cycle (started in 1.0).
  if (flags.stop_timeout.isSome() &&
      flags.stop_timeout.get() > shutdownGracePeriod) {
    shutdownGracePeriod = flags.stop_timeout.get();
  }

  if (flags.launcher_dir.isNone()) {
    EXIT(EXIT_FAILURE) << flags.usage("Missing required option --launcher_dir");
  }

  process::initialize();

  // The 2nd argument for docker create is set to false so we skip
  // validation when creating a docker abstraction, as the slave
  // should have already validated docker.
  Try<Owned<Docker>> docker = Docker::create(
      flags.docker.get(),
      flags.docker_socket.get(),
      false);

  if (docker.isError()) {
    EXIT(EXIT_FAILURE)
      << "Unable to create docker abstraction: " << docker.error();
  }

  Owned<mesos::internal::docker::DockerExecutor> executor(
      new mesos::internal::docker::DockerExecutor(
          docker.get(),
          flags.container.get(),
          flags.sandbox_directory.get(),
          flags.mapped_directory.get(),
          shutdownGracePeriod,
          flags.launcher_dir.get(),
          taskEnvironment,
          defaultContainerDNS,
          flags.cgroups_enable_cfs));

  Owned<mesos::MesosExecutorDriver> driver(
      new mesos::MesosExecutorDriver(executor.get()));

  bool success = driver->run() == mesos::DRIVER_STOPPED;

  // NOTE: We need to delete the executor and driver before we call
  // `process::finalize` because the executor/driver will try to terminate
  // and wait on a libprocess actor in their destructor.
  driver.reset();
  executor.reset();

  // NOTE: We need to finalize libprocess, on Windows especially,
  // as any binary that uses the networking stack on Windows must
  // also clean up the networking stack before exiting.
  process::finalize(true);
  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
