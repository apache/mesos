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

#include "common/validation.hpp"

#include <algorithm>
#include <cctype>

#include <stout/os/constants.hpp>

using std::string;

namespace mesos {
namespace internal {
namespace common {
namespace validation {

Option<Error> validateID(const string& id)
{
  // Rules:
  // - Control charaters are obviously not allowed.
  // - Slashes are disallowed as IDs are likely mapped to directories in Mesos.
  auto invalidCharacter = [](char c) {
    return iscntrl(c) ||
           c == os::POSIX_PATH_SEPARATOR ||
           c == os::WINDOWS_PATH_SEPARATOR;
  };

  if (id.empty()) {
    return Error("ID must be non-empty");
  }

  if (std::any_of(id.begin(), id.end(), invalidCharacter)) {
    return Error("'" + id + "' contains invalid characters");
  }

  return None();
}


// These IDs are valid as long as they meet the common ID requirements
// enforced by `validateID()` but we define each of them separately to
// be clear which IDs are subject to which rules.
Option<Error> validateTaskID(const TaskID& taskId)
{
  return validateID(taskId.value());
}


Option<Error> validateExecutorID(const ExecutorID& executorId)
{
  return validateID(executorId.value());
}


Option<Error> validateSlaveID(const SlaveID& slaveId)
{
  return validateID(slaveId.value());
}


Option<Error> validateFrameworkID(const FrameworkID& frameworkId)
{
  return validateID(frameworkId.value());
}

} // namespace validation {
} // namespace common {
} // namespace internal {
} // namespace mesos {
