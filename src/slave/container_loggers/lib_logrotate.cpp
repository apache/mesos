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

#include <unistd.h>

#include <map>
#include <string>

#include <mesos/mesos.hpp>

#include <mesos/module/container_logger.hpp>

#include <mesos/slave/container_logger.hpp>
#include <mesos/slave/containerizer.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include <stout/bytes.hpp>
#include <stout/error.hpp>
#include <stout/try.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/environment.hpp>
#include <stout/os/fcntl.hpp>
#include <stout/os/killtree.hpp>
#include <stout/os/pipe.hpp>

#ifdef __linux__
#include "linux/systemd.hpp"
#endif // __linux__

#include "slave/container_loggers/logrotate.hpp"
#include "slave/container_loggers/lib_logrotate.hpp"


using namespace mesos;
using namespace process;

using std::array;
using std::map;
using std::string;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLogger;
using mesos::slave::ContainerIO;

namespace mesos {
namespace internal {
namespace logger {

class LogrotateContainerLoggerProcess :
  public Process<LogrotateContainerLoggerProcess>
{
public:
  LogrotateContainerLoggerProcess(const Flags& _flags) : flags(_flags) {}

  // Spawns two subprocesses that read from their stdin and write to
  // "stdout" and "stderr" files in the sandbox.  The subprocesses will rotate
  // the files according to the configured maximum size and number of files.
  Future<ContainerIO> prepare(
      const ContainerID& containerId,
      const ContainerConfig& containerConfig)
  {
    // Prepare the environment for the container logger subprocess.
    // We inherit agent environment variables except for those
    // LIBPROCESS or MESOS prefixed environment variables. See MESOS-6747.
    map<string, string> environment;

    foreachpair (
        const string& key, const string& value, os::environment()) {
      if (!strings::startsWith(key, "LIBPROCESS_") &&
          !strings::startsWith(key, "MESOS_")) {
        environment.emplace(key, value);
      }
    }

    // Make sure the libprocess of the subprocess can properly
    // initialize and find the IP. Since we don't need to use the TCP
    // socket for communication, it's OK to use a local address.
    environment.emplace("LIBPROCESS_IP", "127.0.0.1");

    // Use the number of worker threads for libprocess that was passed
    // in through the flags.
    CHECK_GT(flags.libprocess_num_worker_threads, 0u);
    environment["LIBPROCESS_NUM_WORKER_THREADS"] =
      stringify(flags.libprocess_num_worker_threads);

    // Copy the global rotation flags.
    // These will act as the defaults in case the container's environment
    // overrides a subset of them.
    LoggerFlags overriddenFlags;
    overriddenFlags.max_stdout_size = flags.max_stdout_size;
    overriddenFlags.logrotate_stdout_options = flags.logrotate_stdout_options;
    overriddenFlags.max_stderr_size = flags.max_stderr_size;
    overriddenFlags.logrotate_stderr_options = flags.logrotate_stderr_options;

    // Check for overrides of the rotation settings in the
    // `CommandInfo`s environment variables.
    if (containerConfig.command_info().has_environment()) {
      // Search the environment for prefixed environment variables.
      // We un-prefix those variables before parsing the flag values.
      map<string, string> containerEnvironment;
      foreach (const Environment::Variable& variable,
               containerConfig.command_info().environment().variables()) {
        if (strings::startsWith(
              variable.name(), flags.environment_variable_prefix)) {
          string unprefixed = strings::lower(strings::remove(
              variable.name(),
              flags.environment_variable_prefix,
              strings::PREFIX));
          containerEnvironment[unprefixed] = variable.value();
        }
      }

      // We will error out if there are unknown flags with the same prefix.
      Try<flags::Warnings> load = overriddenFlags.load(containerEnvironment);

      if (load.isError()) {
        return Failure(
            "Failed to load container logger settings: " + load.error());
      }

      // Log any flag warnings.
      foreach (const flags::Warning& warning, load->warnings) {
        LOG(WARNING) << warning.message;
      }
    }

    // NOTE: We manually construct a pipe here instead of using
    // `Subprocess::PIPE` so that the ownership of the FDs is properly
    // represented.  The `Subprocess` spawned below owns the read-end
    // of the pipe and will be solely responsible for closing that end.
    // The ownership of the write-end will be passed to the caller
    // of this function.
    Try<array<int, 2>> pipefd = os::pipe();
    if (pipefd.isError()) {
      return Failure("Failed to create pipe: " + pipefd.error());
    }

    Subprocess::IO::InputFileDescriptors outfds;
    outfds.read = pipefd->at(0);
    outfds.write = pipefd->at(1);

    // Spawn a process to handle stdout.
    mesos::internal::logger::rotate::Flags outFlags;
    outFlags.max_size = overriddenFlags.max_stdout_size;
    outFlags.logrotate_options = overriddenFlags.logrotate_stdout_options;
    outFlags.log_filename = path::join(containerConfig.directory(), "stdout");
    outFlags.logrotate_path = flags.logrotate_path;
    outFlags.user = containerConfig.has_user()
      ? Option<string>(containerConfig.user())
      : Option<string>::none();

    // If we are on systemd, then extend the life of the process as we
    // do with the executor. Any grandchildren's lives will also be
    // extended.
    std::vector<Subprocess::ParentHook> parentHooks;
#ifdef __linux__
    if (systemd::enabled()) {
      parentHooks.emplace_back(Subprocess::ParentHook(
          &systemd::mesos::extendLifetime));
    }
#endif // __linux__

    Try<Subprocess> outProcess = subprocess(
        path::join(flags.launcher_dir, mesos::internal::logger::rotate::NAME),
        {mesos::internal::logger::rotate::NAME},
        Subprocess::FD(outfds.read, Subprocess::IO::OWNED),
        Subprocess::PATH(os::DEV_NULL),
        Subprocess::FD(STDERR_FILENO),
        &outFlags,
        environment,
        None(),
        parentHooks);

    if (outProcess.isError()) {
      os::close(outfds.write.get());
      return Failure("Failed to create logger process: " + outProcess.error());
    }

    // NOTE: We manually construct a pipe here to properly express
    // ownership of the FDs.  See the NOTE above.
    pipefd = os::pipe();
    if (pipefd.isError()) {
      os::close(outfds.write.get());
      os::killtree(outProcess->pid(), SIGKILL);
      return Failure("Failed to create pipe: " + pipefd.error());
    }

    Subprocess::IO::InputFileDescriptors errfds;
    errfds.read = pipefd->at(0);
    errfds.write = pipefd->at(1);

    // Spawn a process to handle stderr.
    mesos::internal::logger::rotate::Flags errFlags;
    errFlags.max_size = overriddenFlags.max_stderr_size;
    errFlags.logrotate_options = overriddenFlags.logrotate_stderr_options;
    errFlags.log_filename = path::join(containerConfig.directory(), "stderr");
    errFlags.logrotate_path = flags.logrotate_path;
    errFlags.user = containerConfig.has_user()
      ? Option<string>(containerConfig.user())
      : Option<string>::none();

    Try<Subprocess> errProcess = subprocess(
        path::join(flags.launcher_dir, mesos::internal::logger::rotate::NAME),
        {mesos::internal::logger::rotate::NAME},
        Subprocess::FD(errfds.read, Subprocess::IO::OWNED),
        Subprocess::PATH(os::DEV_NULL),
        Subprocess::FD(STDERR_FILENO),
        &errFlags,
        environment,
        None(),
        parentHooks);

    if (errProcess.isError()) {
      os::close(outfds.write.get());
      os::close(errfds.write.get());
      os::killtree(outProcess->pid(), SIGKILL);
      return Failure("Failed to create logger process: " + errProcess.error());
    }

    // NOTE: The ownership of these FDs is given to the caller of this function.
    ContainerIO io;
    io.out = ContainerIO::IO::FD(outfds.write.get());
    io.err = ContainerIO::IO::FD(errfds.write.get());
    return io;
  }

protected:
  Flags flags;
};


LogrotateContainerLogger::LogrotateContainerLogger(const Flags& _flags)
  : flags(_flags),
    process(new LogrotateContainerLoggerProcess(flags))
{
  // Spawn and pass validated parameters to the process.
  spawn(process.get());
}


LogrotateContainerLogger::~LogrotateContainerLogger()
{
  terminate(process.get());
  wait(process.get());
}


Try<Nothing> LogrotateContainerLogger::initialize()
{
  return Nothing();
}


Future<ContainerIO> LogrotateContainerLogger::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  return dispatch(
      process.get(),
      &LogrotateContainerLoggerProcess::prepare,
      containerId,
      containerConfig);
}

} // namespace logger {
} // namespace internal {
} // namespace mesos {


mesos::modules::Module<ContainerLogger>
org_apache_mesos_LogrotateContainerLogger(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "Logrotate Container Logger module.",
    nullptr,
    [](const Parameters& parameters) -> ContainerLogger* {
      // Convert `parameters` into a map.
      map<string, string> values;
      foreach (const Parameter& parameter, parameters.parameter()) {
        values[parameter.key()] = parameter.value();
      }

      // Load and validate flags from the map.
      mesos::internal::logger::Flags flags;
      Try<flags::Warnings> load = flags.load(values);

      if (load.isError()) {
        LOG(ERROR) << "Failed to parse parameters: " << load.error();
        return nullptr;
      }

      // Log any flag warnings.
      foreach (const flags::Warning& warning, load->warnings) {
        LOG(WARNING) << warning.message;
      }

      return new mesos::internal::logger::LogrotateContainerLogger(flags);
    });
