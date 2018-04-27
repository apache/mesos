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

#ifndef __MASTER_ALLOCATOR_SORTER_DRF_SORTER_METRICS_HPP__
#define __MASTER_ALLOCATOR_SORTER_DRF_SORTER_METRICS_HPP__

#include <string>

#include <process/pid.hpp>

#include <process/metrics/pull_gauge.hpp>

#include <stout/hashmap.hpp>

namespace mesos {
namespace internal {
namespace master {
namespace allocator {

class DRFSorter;

struct Metrics
{
  explicit Metrics(
      const process::UPID& context,
      DRFSorter& sorter,
      const std::string& prefix);

  ~Metrics();

  void add(const std::string& client);
  void remove(const std::string& client);

  const process::UPID context;

  DRFSorter* sorter;

  const std::string prefix;

  // Dominant share of each client.
  hashmap<std::string, process::metrics::PullGauge> dominantShares;
};

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif  // __MASTER_ALLOCATOR_SORTER_DRF_SORTER_METRICS_HPP__
