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

#include "logging/flags.hpp"


mesos::internal::logging::Flags::Flags()
{
  add(&Flags::quiet,
      "quiet",
      "Disable logging to stderr.",
      false);

  add(&Flags::logging_level,
      "logging_level",
      "Log message at or above this level.\n"
      "Possible values: `INFO`, `WARNING`, `ERROR`.\n"
      "If `--quiet` is specified, this will only affect the logs\n"
      "written to `--log_dir`, if specified.",
      "INFO");

  add(&Flags::log_dir,
      "log_dir",
      "Location to put log files.  By default, nothing is written to disk.\n"
      "Does not affect logging to stderr.\n"
      "If specified, the log file will appear in the Mesos WebUI.\n"
      "NOTE: 3rd party log messages (e.g. ZooKeeper) are\n"
      "only written to stderr!");

  add(&Flags::logbufsecs,
      "logbufsecs",
      "Maximum number of seconds that logs may be buffered for.\n"
      "By default, logs are flushed immediately.",
      0);

  add(&Flags::initialize_driver_logging,
      "initialize_driver_logging",
      "Whether the master/agent should initialize Google logging for the\n"
      "scheduler and executor drivers, in the same way as described here.\n"
      "The scheduler/executor drivers have separate logs and do not get\n"
      "written to the master/agent logs.\n\n"
      "This option has no effect when using the HTTP scheduler/executor APIs.\n"
      "By default, this option is true.",
      true);

  add(&Flags::external_log_file,
      "external_log_file",
      "Location of the externally managed log file.  Mesos does not write to\n"
      "this file directly and merely exposes it in the WebUI and HTTP API.\n"
      "This is only useful when logging to stderr in combination with an\n"
      "external logging mechanism, like syslog or journald.\n\n"
      "This option is meaningless when specified along with `--quiet`.\n\n"
      "This option takes precedence over `--log_dir` in the WebUI.\n"
      "However, logs will still be written to the `--log_dir` if\n"
      "that option is specified.");
}
