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

#include <stout/os/environment.hpp>
#include <stout/os/fcntl.hpp>
#include <stout/os/killtree.hpp>

#ifdef __linux__
#include "linux/systemd.hpp"
#endif // __linux__

#include "slave/container_loggers/logrotate.hpp"
#include "slave/container_loggers/lib_logrotate.hpp"


using namespace mesos;
using namespace process;

using mesos::slave::ContainerLogger;

namespace mesos {
namespace internal {
namespace logger {

using SubprocessInfo = ContainerLogger::SubprocessInfo;


class LogrotateContainerLoggerProcess :
  public Process<LogrotateContainerLoggerProcess>
{
public:
  LogrotateContainerLoggerProcess(const Flags& _flags) : flags(_flags) {}

  // Spawns two subprocesses that read from their stdin and write to
  // "stdout" and "stderr" files in the sandbox.  The subprocesses will rotate
  // the files according to the configured maximum size and number of files.
  Future<SubprocessInfo> prepare(
      const ExecutorInfo& executorInfo,
      const std::string& sandboxDirectory,
      const Option<std::string>& user)
  {
    // Inherit most, but not all of the agent's environment.
    // Since the subprocess links to libmesos, it will need some of the
    // same environment used to launch the agent (also uses libmesos).
    // The libprocess port is explicitly removed because this
    // will conflict with the already-running agent.
    std::map<std::string, std::string> environment = os::environment();
    environment.erase("LIBPROCESS_PORT");
    environment.erase("LIBPROCESS_ADVERTISE_PORT");

    // Use the number of worker threads for libprocess that was passed
    // in through the flags.
    CHECK_GT(flags.libprocess_num_worker_threads, 0u);
    environment["LIBPROCESS_NUM_WORKER_THREADS"] =
      stringify(flags.libprocess_num_worker_threads);

    // Copy the global rotation flags.
    // These will act as the defaults in case the executor environment
    // overrides a subset of them.
    LoggerFlags overridenFlags;
    overridenFlags.max_stdout_size = flags.max_stdout_size;
    overridenFlags.logrotate_stdout_options = flags.logrotate_stdout_options;
    overridenFlags.max_stderr_size = flags.max_stderr_size;
    overridenFlags.logrotate_stderr_options = flags.logrotate_stderr_options;

    // Check for overrides of the rotation settings in the
    // `ExecutorInfo`s environment variables.
    if (executorInfo.has_command() &&
        executorInfo.command().has_environment()) {
      // Search the environment for prefixed environment variables.
      // We un-prefix those variables before parsing the flag values.
      std::map<std::string, std::string> executorEnvironment;
      foreach (const Environment::Variable variable,
               executorInfo.command().environment().variables()) {
        if (strings::startsWith(
              variable.name(), flags.environment_variable_prefix)) {
          std::string unprefixed = strings::lower(strings::remove(
              variable.name(),
              flags.environment_variable_prefix,
              strings::PREFIX));
          executorEnvironment[unprefixed] = variable.value();
        }
      }

      // We will error out if there are unknown flags with the same prefix.
      Try<flags::Warnings> load = overridenFlags.load(executorEnvironment);

      if (load.isError()) {
        return Failure(
            "Failed to load executor logger settings: " + load.error());
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
    int pipefd[2];
    if (::pipe(pipefd) == -1) {
      return Failure(ErrnoError("Failed to create pipe").message);
    }

    Subprocess::IO::InputFileDescriptors outfds;
    outfds.read = pipefd[0];
    outfds.write = pipefd[1];

    // NOTE: We need to `cloexec` this FD so that it will be closed when
    // the child subprocess is spawned and so that the FD will not be
    // inherited by the second child for stderr.
    Try<Nothing> cloexec = os::cloexec(outfds.write.get());
    if (cloexec.isError()) {
      os::close(outfds.read);
      os::close(outfds.write.get());
      return Failure("Failed to cloexec: " + cloexec.error());
    }

    // Spawn a process to handle stdout.
    mesos::internal::logger::rotate::Flags outFlags;
    outFlags.max_size = overridenFlags.max_stdout_size;
    outFlags.logrotate_options = overridenFlags.logrotate_stdout_options;
    outFlags.log_filename = path::join(sandboxDirectory, "stdout");
    outFlags.logrotate_path = flags.logrotate_path;
    outFlags.user = user;

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
        Subprocess::PATH("/dev/null"),
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
    if (::pipe(pipefd) == -1) {
      os::close(outfds.write.get());
      os::killtree(outProcess.get().pid(), SIGKILL);
      return Failure(ErrnoError("Failed to create pipe").message);
    }

    Subprocess::IO::InputFileDescriptors errfds;
    errfds.read = pipefd[0];
    errfds.write = pipefd[1];

    // NOTE: We need to `cloexec` this FD so that it will be closed when
    // the child subprocess is spawned.
    cloexec = os::cloexec(errfds.write.get());
    if (cloexec.isError()) {
      os::close(outfds.write.get());
      os::close(errfds.read);
      os::close(errfds.write.get());
      os::killtree(outProcess.get().pid(), SIGKILL);
      return Failure("Failed to cloexec: " + cloexec.error());
    }

    // Spawn a process to handle stderr.
    mesos::internal::logger::rotate::Flags errFlags;
    errFlags.max_size = overridenFlags.max_stderr_size;
    errFlags.logrotate_options = overridenFlags.logrotate_stderr_options;
    errFlags.log_filename = path::join(sandboxDirectory, "stderr");
    errFlags.logrotate_path = flags.logrotate_path;
    errFlags.user = user;

    Try<Subprocess> errProcess = subprocess(
        path::join(flags.launcher_dir, mesos::internal::logger::rotate::NAME),
        {mesos::internal::logger::rotate::NAME},
        Subprocess::FD(errfds.read, Subprocess::IO::OWNED),
        Subprocess::PATH("/dev/null"),
        Subprocess::FD(STDERR_FILENO),
        &errFlags,
        environment,
        None(),
        parentHooks);

    if (errProcess.isError()) {
      os::close(outfds.write.get());
      os::close(errfds.write.get());
      os::killtree(outProcess.get().pid(), SIGKILL);
      return Failure("Failed to create logger process: " + errProcess.error());
    }

    // NOTE: The ownership of these FDs is given to the caller of this function.
    ContainerLogger::SubprocessInfo info;
    info.out = SubprocessInfo::IO::FD(outfds.write.get());
    info.err = SubprocessInfo::IO::FD(errfds.write.get());
    return info;
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


Future<SubprocessInfo> LogrotateContainerLogger::prepare(
    const ExecutorInfo& executorInfo,
    const std::string& sandboxDirectory,
    const Option<std::string>& user)
{
  return dispatch(
      process.get(),
      &LogrotateContainerLoggerProcess::prepare,
      executorInfo,
      sandboxDirectory,
      user);
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
      std::map<std::string, std::string> values;
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
