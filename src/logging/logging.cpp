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

#include <glog/logging.h>

#include <process/once.hpp>

#include "common/utils.hpp"

#include "logging/logging.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace logging {

Flags flags;


void registerOptions(Configurator* configurator)
{
  configurator->addOption<bool>(
      "quiet",
      'q',
      "Disable logging to stderr (default: false)",
      false);

  configurator->addOption<string>(
      "log_dir",
      "Location to put log files (no default, nothing"
      " is written to disk unless specified; "
      " does not affect logging to stderr)");

  configurator->addOption<int>(
      "logbufsecs",
      "How many seconds to buffer log messages for (default: 0)",
      0);
}


void initialize(const string& _argv0, const Configuration& conf)
{
  static process::Once initialized;

  if (initialized.once()) {
    return;
  }

  // Persistent copy of argv0 since InitGoogleLogging requires the
  // string we pass to it to be accessible indefinitely.
  static string argv0 = _argv0;

  Option<string> directory = conf.get<string>("log_dir");

  // Set glog's parameters through Google Flags variables.
  if (directory.isSome()) {
    if (!utils::os::mkdir(directory.get())) {
      std::cerr << "Could not initialize logging: Failed to create directory "
                << directory.get() << std::endl;
      exit(1);
    }
    FLAGS_log_dir = directory.get();
  }


  // Log everything to stderr IN ADDITION to log files unless
  // otherwise specified.
  bool quiet = conf.get<bool>("quiet", false);

  if (!quiet) {
    FLAGS_stderrthreshold = 0; // INFO.
  }

  FLAGS_logbufsecs = conf.get<int>("logbufsecs", 0);

  google::InitGoogleLogging(argv0.c_str());

  LOG(INFO) << "Logging to " <<
    (directory.isSome() ? directory.get() : "STDERR");

  initialized.done();
}


void initialize(const string& _argv0, const Flags& _flags)
{
  static process::Once initialized;

  if (initialized.once()) {
    return;
  }

  // Persistent copy of argv0 since InitGoogleLogging requires the
  // string we pass to it to be accessible indefinitely.
  static string argv0 = _argv0;

  // Save the flags for use in other places.
  flags = _flags;

  // Set glog's parameters through Google Flags variables.
  if (flags.log_dir.isSome()) {
    if (!utils::os::mkdir(flags.log_dir.get())) {
      std::cerr << "Could not initialize logging: Failed to create directory "
                << flags.log_dir.get() << std::endl;
      exit(1);
    }
    FLAGS_log_dir = flags.log_dir.get();
  }


  // Log everything to stderr IN ADDITION to log files unless
  // otherwise specified.
  if (!flags.quiet) {
    FLAGS_stderrthreshold = 0; // INFO.
  }

  FLAGS_logbufsecs = flags.logbufsecs;

  google::InitGoogleLogging(argv0.c_str());

  LOG(INFO) << "Logging to " <<
    (flags.log_dir.isSome() ? flags.log_dir.get() : "STDERR");

  initialized.done();
}

} // namespace logging {
} // namespace internal {
} // namespace mesos {
