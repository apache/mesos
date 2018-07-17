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

#ifndef __SLAVE_CONTAINER_LOGGER_LIB_LOGROTATE_HPP__
#define __SLAVE_CONTAINER_LOGGER_LIB_LOGROTATE_HPP__

#include <stdio.h>

#include <mesos/slave/container_logger.hpp>
#include <mesos/slave/containerizer.hpp>

#include <stout/bytes.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/exists.hpp>
#include <stout/os/pagesize.hpp>
#include <stout/os/shell.hpp>

#include "slave/container_loggers/logrotate.hpp"

namespace mesos {
namespace internal {
namespace logger {

// Forward declaration.
class LogrotateContainerLoggerProcess;


// These flags are loaded twice: once when the `ContainerLogger` module
// is created and each time before launching containers. The flags loaded
// at module creation act as global default values, whereas flags loaded
// prior to containers can override the global values.
struct LoggerFlags : public virtual flags::FlagsBase
{
  LoggerFlags()
  {
    add(&LoggerFlags::max_stdout_size,
        "max_stdout_size",
        "Maximum size, in bytes, of a single stdout log file.\n"
        "Defaults to 10 MB.  Must be at least 1 (memory) page.",
        Megabytes(10),
        &LoggerFlags::validateSize);

    add(&LoggerFlags::logrotate_stdout_options,
        "logrotate_stdout_options",
        "Additional config options to pass into 'logrotate' for stdout.\n"
        "This string will be inserted into a 'logrotate' configuration file.\n"
        "i.e.\n"
        "  /path/to/stdout {\n"
        "    <logrotate_stdout_options>\n"
        "    size <max_stdout_size>\n"
        "  }\n"
        "NOTE: The 'size' option will be overridden by this module.");

    add(&LoggerFlags::max_stderr_size,
        "max_stderr_size",
        "Maximum size, in bytes, of a single stderr log file.\n"
        "Defaults to 10 MB.  Must be at least 1 (memory) page.",
        Megabytes(10),
        &LoggerFlags::validateSize);

    add(&LoggerFlags::logrotate_stderr_options,
        "logrotate_stderr_options",
        "Additional config options to pass into 'logrotate' for stderr.\n"
        "This string will be inserted into a 'logrotate' configuration file.\n"
        "i.e.\n"
        "  /path/to/stderr {\n"
        "    <logrotate_stderr_options>\n"
        "    size <max_stderr_size>\n"
        "  }\n"
        "NOTE: The 'size' option will be overridden by this module.");
  }

  static Option<Error> validateSize(const Bytes& value)
  {
    if (value.bytes() < os::pagesize()) {
      return Error(
          "Expected --max_stdout_size and --max_stderr_size of "
          "at least " + stringify(os::pagesize()) + " bytes");
    }

    return None();
  }

  Bytes max_stdout_size;
  Option<std::string> logrotate_stdout_options;

  Bytes max_stderr_size;
  Option<std::string> logrotate_stderr_options;
};


struct Flags : public virtual LoggerFlags
{
  Flags()
  {
    add(&Flags::environment_variable_prefix,
        "environment_variable_prefix",
        "Prefix for environment variables meant to modify the behavior of\n"
        "the logrotate logger for the specific container being launched.\n"
        "The logger will look for four prefixed environment variables in the\n"
        "container's 'CommandInfo's 'Environment':\n"
        "  * MAX_STDOUT_SIZE\n"
        "  * LOGROTATE_STDOUT_OPTIONS\n"
        "  * MAX_STDERR_SIZE\n"
        "  * LOGROTATE_STDERR_OPTIONS\n"
        "If present, these variables will overwrite the global values set\n"
        "via module parameters.",
        "CONTAINER_LOGGER_");

    add(&Flags::launcher_dir,
        "launcher_dir",
        "Directory path of Mesos binaries.  The logrotate container logger\n"
        "will find the '" + mesos::internal::logger::rotate::NAME + "'\n"
        "binary file under this directory.",
        PKGLIBEXECDIR,
        [](const std::string& value) -> Option<Error> {
          std::string executablePath =
            path::join(value, mesos::internal::logger::rotate::NAME);

          if (!os::exists(executablePath)) {
            return Error("Cannot find: " + executablePath);
          }

          return None();
        });

    add(&Flags::logrotate_path,
        "logrotate_path",
        "If specified, the logrotate container logger will use the specified\n"
        "'logrotate' instead of the system's 'logrotate'.",
        "logrotate",
        [](const std::string& value) -> Option<Error> {
          // Check if `logrotate` exists via the help command.
          // TODO(josephw): Consider a more comprehensive check.
          Try<std::string> helpCommand =
            os::shell(value + " --help > " + os::DEV_NULL);

          if (helpCommand.isError()) {
            return Error(
                "Failed to check logrotate: " + helpCommand.error());
          }

          return None();
        });

    add(&Flags::libprocess_num_worker_threads,
        "libprocess_num_worker_threads",
        "Number of Libprocess worker threads.\n"
        "Defaults to 8.  Must be at least 1.",
        8u,
        [](const size_t& value) -> Option<Error> {
          if (value < 1u) {
            return Error(
                "Expected --libprocess_num_worker_threads of at least 1");
          }

          return None();
        });
  }

  std::string environment_variable_prefix;

  std::string launcher_dir;
  std::string logrotate_path;

  size_t libprocess_num_worker_threads;
};


// The `LogrotateContainerLogger` is a container logger that utilizes the
// `logrotate` utility to strictly constrain total size of a container's
// stdout and stderr log files.  All `logrotate` configuration options
// (besides `size`, which this module uses) are supported.  See `Flags` above.
class LogrotateContainerLogger : public mesos::slave::ContainerLogger
{
public:
  LogrotateContainerLogger(const Flags& _flags);

  ~LogrotateContainerLogger() override;

  // This is a noop. The logrotate container logger has nothing to initialize.
  Try<Nothing> initialize() override;

  process::Future<mesos::slave::ContainerIO> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig) override;

protected:
  Flags flags;
  process::Owned<LogrotateContainerLoggerProcess> process;
};

} // namespace logger {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CONTAINER_LOGGER_LIB_LOGROTATE_HPP__
