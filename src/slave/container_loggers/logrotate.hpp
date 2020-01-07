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

#ifndef __SLAVE_CONTAINER_LOGGER_LOGROTATE_HPP__
#define __SLAVE_CONTAINER_LOGGER_LOGROTATE_HPP__

#include <stdio.h>
#include <unistd.h>

#include <stout/bytes.hpp>
#include <stout/error.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/pagesize.hpp>


namespace mesos {
namespace internal {
namespace logger {
namespace rotate {

const std::string NAME = "mesos-logrotate-logger";
const std::string CONF_SUFFIX = ".logrotate.conf";
const std::string STATE_SUFFIX = ".logrotate.state";

struct Flags : public virtual flags::FlagsBase
{
  Flags()
  {
    setUsageMessage(
      "Usage: " + NAME + " [options]\n"
      "\n"
      "This command pipes from STDIN to the given leading log file.\n"
      "When the leading log file reaches '--max_size', the command.\n"
      "uses 'logrotate' to rotate the logs.  All 'logrotate' options\n"
      "are supported.  See '--logrotate_options'.\n"
      "\n");

    add(&Flags::max_size,
        "max_size",
        "Maximum size, in bytes, of a single log file.\n"
        "Defaults to 10 MB.  Must be at least 1 (memory) page.",
        Megabytes(10),
        [](const Bytes& value) -> Option<Error> {
          if (value.bytes() < os::pagesize()) {
            return Error(
                "Expected --max_size of at least " +
                stringify(os::pagesize()) + " bytes");
          }
          return None();
        });

    add(&Flags::logrotate_options,
        "logrotate_options",
        "Additional config options to pass into 'logrotate'.\n"
        "This string will be inserted into a 'logrotate' configuration file.\n"
        "i.e.\n"
        "  /path/to/<log_filename> {\n"
        "    <logrotate_options>\n"
        "    size <max_size>\n"
        "  }\n"
        "NOTE: The 'size' option will be overridden by this command.");

    add(&Flags::log_filename,
        "log_filename",
        "Absolute path to the leading log file.\n"
        "NOTE: This command will also create two files by appending\n"
        "'" + CONF_SUFFIX + "' and '" + STATE_SUFFIX + "' to the end of\n"
        "'--log_filename'.  These files are used by 'logrotate'.",
        [](const Option<std::string>& value) -> Option<Error> {
          if (value.isNone()) {
            return Error("Missing required option --log_filename");
          }

          if (!path::is_absolute(value.get())) {
            return Error("Expected --log_filename to be an absolute path");
          }

          return None();
        });

    add(&Flags::logrotate_path,
        "logrotate_path",
        "If specified, this command will use the specified\n"
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

    add(&Flags::user,
        "user",
        "The user this command should run as.");
  }

  Bytes max_size;
  Option<std::string> logrotate_options;
  Option<std::string> log_filename;
  std::string logrotate_path;
  Option<std::string> user;
};

} // namespace rotate {
} // namespace logger {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CONTAINER_LOGGER_LOGROTATE_HPP__
