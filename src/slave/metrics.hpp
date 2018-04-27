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
#include <process/metrics/pull_gauge.hpp>


namespace mesos {
namespace internal {
namespace slave {

class Slave;

struct Metrics
{
  explicit Metrics(const Slave& slave);

  ~Metrics();

  void setRecoveryTime(const Duration& duration);

  process::metrics::PullGauge uptime_secs;
  process::metrics::PullGauge registered;

  process::metrics::Counter recovery_errors;
  Option<process::metrics::PullGauge> recovery_time_secs;

  process::metrics::PullGauge frameworks_active;

  process::metrics::PullGauge tasks_staging;
  process::metrics::PullGauge tasks_starting;
  process::metrics::PullGauge tasks_running;
  process::metrics::PullGauge tasks_killing;
  process::metrics::Counter tasks_finished;
  process::metrics::Counter tasks_failed;
  process::metrics::Counter tasks_killed;
  process::metrics::Counter tasks_lost;
  process::metrics::Counter tasks_gone;

  process::metrics::PullGauge executors_registering;
  process::metrics::PullGauge executors_running;
  process::metrics::PullGauge executors_terminating;
  process::metrics::Counter executors_terminated;
  process::metrics::Counter executors_preempted;

  process::metrics::Counter valid_status_updates;
  process::metrics::Counter invalid_status_updates;

  process::metrics::Counter valid_framework_messages;
  process::metrics::Counter invalid_framework_messages;

  process::metrics::PullGauge executor_directory_max_allowed_age_secs;

  process::metrics::Counter container_launch_errors;

  // Non-revocable resources.
  std::vector<process::metrics::PullGauge> resources_total;
  std::vector<process::metrics::PullGauge> resources_used;
  std::vector<process::metrics::PullGauge> resources_percent;

  // Revocable resources.
  std::vector<process::metrics::PullGauge> resources_revocable_total;
  std::vector<process::metrics::PullGauge> resources_revocable_used;
  std::vector<process::metrics::PullGauge> resources_revocable_percent;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_METRICS_HPP__
