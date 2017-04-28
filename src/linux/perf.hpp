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

#ifndef __PERF_HPP__
#define __PERF_HPP__

#include <unistd.h>

#include <set>
#include <string>

#include <process/future.hpp>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/version.hpp>

// For PerfStatistics protobuf.
#include "mesos/mesos.hpp"

namespace perf {

// Sample the perf events for process(es) in the perf_event cgroups
// for duration. The returned hashmap is keyed by cgroup.
// NOTE: cgroups should be relative to the perf_event subsystem mount,
// e.g., mesos/test for /sys/fs/cgroup/perf_event/mesos/test.
process::Future<hashmap<std::string, mesos::PerfStatistics>> sample(
    const std::set<std::string>& events,
    const std::set<std::string>& cgroups,
    const Duration& duration);


// Validate a set of events are accepted by `perf stat`.
bool valid(const std::set<std::string>& events);


// Returns whether perf is supported on this host. Returns false if
// the kernel is too old (requires >= 2.6.39).
bool supported();


// Returns the detected perf version. Exposed for testing.
process::Future<Version> version();


// Parse a perf(1) version string. Exposed for testing.
Try<Version> parseVersion(const std::string& output);


// Note: The parse function is exposed to allow testing of the
// multiple supported perf stat output formats.
Try<hashmap<std::string, mesos::PerfStatistics>> parse(
    const std::string& output);

} // namespace perf {

#endif // __PERF_HPP__
