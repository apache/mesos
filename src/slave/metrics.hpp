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

#ifndef __SLAVE_METRICS_HPP__
#define __SLAVE_METRICS_HPP__

#include <vector>

#include <process/metrics/counter.hpp>
#include <process/metrics/gauge.hpp>


namespace mesos {
namespace internal {
namespace slave {

class Slave;

struct Metrics
{
  explicit Metrics(const Slave& slave);

  ~Metrics();

  process::metrics::Gauge uptime_secs;
  process::metrics::Gauge registered;

  process::metrics::Counter recovery_errors;

  process::metrics::Gauge frameworks_active;

  process::metrics::Gauge tasks_staging;
  process::metrics::Gauge tasks_starting;
  process::metrics::Gauge tasks_running;
  process::metrics::Gauge tasks_killing;
  process::metrics::Counter tasks_finished;
  process::metrics::Counter tasks_failed;
  process::metrics::Counter tasks_killed;
  process::metrics::Counter tasks_lost;
  process::metrics::Counter tasks_gone;

  process::metrics::Gauge executors_registering;
  process::metrics::Gauge executors_running;
  process::metrics::Gauge executors_terminating;
  process::metrics::Counter executors_terminated;
  process::metrics::Counter executors_preempted;

  process::metrics::Counter valid_status_updates;
  process::metrics::Counter invalid_status_updates;

  process::metrics::Counter valid_framework_messages;
  process::metrics::Counter invalid_framework_messages;

  process::metrics::Gauge executor_directory_max_allowed_age_secs;

  process::metrics::Counter container_launch_errors;

  // Non-revocable resources.
  std::vector<process::metrics::Gauge> resources_total;
  std::vector<process::metrics::Gauge> resources_used;
  std::vector<process::metrics::Gauge> resources_percent;

  // Revocable resources.
  std::vector<process::metrics::Gauge> resources_revocable_total;
  std::vector<process::metrics::Gauge> resources_revocable_used;
  std::vector<process::metrics::Gauge> resources_revocable_percent;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_METRICS_HPP__
