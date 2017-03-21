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

#ifndef __CGROUPS_ISOLATOR_CONSTANTS_HPP__
#define __CGROUPS_ISOLATOR_CONSTANTS_HPP__

#include <string>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>

namespace mesos {
namespace internal {
namespace slave {

// CPU subsystem constants.
const uint64_t CPU_SHARES_PER_CPU = 1024;
const uint64_t CPU_SHARES_PER_CPU_REVOCABLE = 10;
const uint64_t MIN_CPU_SHARES = 2; // Linux constant.
const Duration CPU_CFS_PERIOD = Milliseconds(100); // Linux default.
const Duration MIN_CPU_CFS_QUOTA = Milliseconds(1);


// Memory subsystem constants.
const Bytes MIN_MEMORY = Megabytes(32);


// Subsystem names.
const std::string CGROUP_SUBSYSTEM_BLKIO_NAME = "blkio";
const std::string CGROUP_SUBSYSTEM_CPU_NAME = "cpu";
const std::string CGROUP_SUBSYSTEM_CPUACCT_NAME = "cpuacct";
const std::string CGROUP_SUBSYSTEM_CPUSET_NAME = "cpuset";
const std::string CGROUP_SUBSYSTEM_DEVICES_NAME = "devices";
const std::string CGROUP_SUBSYSTEM_HUGETLB_NAME = "hugetlb";
const std::string CGROUP_SUBSYSTEM_MEMORY_NAME = "memory";
const std::string CGROUP_SUBSYSTEM_NET_CLS_NAME = "net_cls";
const std::string CGROUP_SUBSYSTEM_NET_PRIO_NAME = "net_prio";
const std::string CGROUP_SUBSYSTEM_PERF_EVENT_NAME = "perf_event";
const std::string CGROUP_SUBSYSTEM_PIDS_NAME = "pids";

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __CGROUPS_ISOLATOR_CONSTANTS_HPP__
