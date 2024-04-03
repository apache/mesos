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

#ifndef __CGROUPS_V2_ISOLATOR_CONSTANTS_HPP__
#define __CGROUPS_V2_ISOLATOR_CONSTANTS_HPP__

#include <string>

#include <stout/duration.hpp>

namespace mesos {
namespace internal {
namespace slave {

// CPU controller constants.
const uint64_t CPU_SHARES_PER_CPU = 1024;
const uint64_t CPU_SHARES_PER_CPU_REVOCABLE = 10;
const uint64_t MIN_CPU_SHARES = 2; // Linux constant.
const Duration CPU_CFS_PERIOD = Milliseconds(100); // Linux default.
const Duration MIN_CPU_CFS_QUOTA = Milliseconds(1);

const std::string CGROUPS_V2_CONTROLLER_CORE_NAME =
  "core"; // Interfaces with "cgroup.*" control files
const std::string CGROUPS_V2_CONTROLLER_CPU_NAME = "cpu";

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __CGROUPS_V2_ISOLATOR_CONSTANTS_HPP__
