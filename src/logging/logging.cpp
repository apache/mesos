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

void initialize(const string& _argv0, const Flags& flags)
{
  static process::Once initialized;

  if (initialized.once()) {
    return;
  }

  // Persistent copy of argv0 since InitGoogleLogging requires the
  // string we pass to it to be accessible indefinitely.
  static string argv0 = _argv0;

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
