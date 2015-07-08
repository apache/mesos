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

#include "logging/flags.hpp"


mesos::internal::logging::Flags::Flags()
{
  add(&Flags::quiet,
      "quiet",
      "Disable logging to stderr",
      false);

  add(&Flags::logging_level,
      "logging_level",
      "Log message at or above this level; possible values: \n"
      "'INFO', 'WARNING', 'ERROR'; if quiet flag is used, this \n"
      "will affect just the logs from log_dir (if specified)",
      "INFO");

  add(&Flags::log_dir,
      "log_dir",
      "Directory path to put log files (no default, nothing\n"
      "is written to disk unless specified;\n"
      "does not affect logging to stderr).\n"
      "NOTE: 3rd party log messages (e.g. ZooKeeper) are\n"
      "only written to stderr!");

  add(&Flags::logbufsecs,
      "logbufsecs",
      "How many seconds to buffer log messages for",
      0);

  add(&Flags::initialize_driver_logging,
      "initialize_driver_logging",
      "Whether to automatically initialize Google logging of scheduler\n"
      "and/or executor drivers.",
      true);

  add(&Flags::external_log_file,
      "external_log_file",
      "Specified the externally managed log file. This file will be\n"
      "exposed in the webui and HTTP api. This is useful when using\n"
      "stderr logging as the log file is otherwise unknown to Mesos.");
}
