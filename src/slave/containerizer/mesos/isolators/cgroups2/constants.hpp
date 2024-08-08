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
const uint64_t CGROUPS2_CPU_WEIGHT_PER_CPU = 100;
const uint64_t CGROUPS2_CPU_WEIGHT_PER_CPU_REVOCABLE = 1;
const uint64_t CGROUPS2_MIN_CPU_WEIGHT = 1; // Linux constant.
const Duration CGROUPS2_CPU_CFS_PERIOD = Milliseconds(100); // Linux default.
const Duration CGROUPS2_MIN_CPU_CFS_QUOTA = Milliseconds(1);

// Memory controller constants.
const Bytes CGROUPS2_MIN_MEMORY = Megabytes(32);

const std::string CGROUPS2_CONTROLLER_CORE_NAME = "core";
const std::string CGROUPS2_CONTROLLER_CPU_NAME = "cpu";
const std::string CGROUPS2_CONTROLLER_MEMORY_NAME = "memory";
const std::string CGROUPS2_CONTROLLER_PERF_EVENT_NAME = "perf_event";
const std::string CGROUPS2_CONTROLLER_DEVICES_NAME = "devices";
const std::string CGROUPS2_CONTROLLER_IO_NAME = "io";
const std::string CGROUPS2_CONTROLLER_HUGETLB_NAME = "hugetlb";
const std::string CGROUPS2_CONTROLLER_CPUSET_NAME = "cpuset";
const std::string CGROUPS2_CONTROLLER_PIDS_NAME = "pids";

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __CGROUPS_V2_ISOLATOR_CONSTANTS_HPP__
