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

#ifndef __LOGGING_LOGGING_HPP__
#define __LOGGING_LOGGING_HPP__

#include <string>

#include <glog/logging.h> // Includes LOG(*), PLOG(*), CHECK, etc.

#include "logging/flags.hpp"

namespace mesos {
namespace internal {
namespace logging {

void initialize(
    const std::string& argv0,
    const Flags& flags,
    bool installFailureSignalHandler = false);


// Returns the log file for the provided severity level.
// LogSeverity is one of {INFO, WARNING, ERROR}.
Try<std::string> getLogFile(google::LogSeverity severity);


// Returns the provided logging level as a LogSeverity type.
google::LogSeverity getLogSeverity(const std::string& logging_level);

} // namespace logging {
} // namespace internal {
} // namespace mesos {

#endif // __LOGGING_LOGGING_HPP__
