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

#include <string>

#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/foreach.hpp>

#include "slave/metrics.hpp"
#include "slave/slave.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace slave {

using process::metrics::PullGauge;

Metrics::Metrics(const Slave& slave)
  : uptime_secs(
        "slave/uptime_secs",
        defer(slave, &Slave::_uptime_secs)),
    registered(
        "slave/registered",
        defer(slave, &Slave::_registered)),
    recovery_errors(
        "slave/recovery_errors"),
    frameworks_active(
        "slave/frameworks_active",
        defer(slave, &Slave::_frameworks_active)),
    tasks_staging(
        "slave/tasks_staging",
        defer(slave, &Slave::_tasks_staging)),
    tasks_starting(
        "slave/tasks_starting",
        defer(slave, &Slave::_tasks_starting)),
    tasks_running(
        "slave/tasks_running",
        defer(slave, &Slave::_tasks_running)),
    tasks_killing(
        "slave/tasks_killing",
        defer(slave, &Slave::_tasks_killing)),
    tasks_finished(
        "slave/tasks_finished"),
    tasks_failed(
        "slave/tasks_failed"),
    tasks_killed(
        "slave/tasks_killed"),
    tasks_lost(
        "slave/tasks_lost"),
    tasks_gone(
        "slave/tasks_gone"),
    executors_registering(
        "slave/executors_registering",
        defer(slave, &Slave::_executors_registering)),
    executors_running(
        "slave/executors_running",
        defer(slave, &Slave::_executors_running)),
    executors_terminating(
        "slave/executors_terminating",
        defer(slave, &Slave::_executors_terminating)),
    executors_terminated(
        "slave/executors_terminated"),
    executors_preempted(
        "slave/executors_preempted"),
    valid_status_updates(
        "slave/valid_status_updates"),
    invalid_status_updates(
        "slave/invalid_status_updates"),
    valid_framework_messages(
        "slave/valid_framework_messages"),
    invalid_framework_messages(
        "slave/invalid_framework_messages"),
    executor_directory_max_allowed_age_secs(
        "slave/executor_directory_max_allowed_age_secs",
        defer(slave, &Slave::_executor_directory_max_allowed_age_secs)),
    container_launch_errors(
        "slave/container_launch_errors")
{
  // TODO(dhamon): Check return values for metric registration.
  process::metrics::add(uptime_secs);
  process::metrics::add(registered);

  process::metrics::add(recovery_errors);

  process::metrics::add(frameworks_active);

  process::metrics::add(tasks_staging);
  process::metrics::add(tasks_starting);
  process::metrics::add(tasks_running);
  process::metrics::add(tasks_killing);
  process::metrics::add(tasks_finished);
  process::metrics::add(tasks_failed);
  process::metrics::add(tasks_killed);
  process::metrics::add(tasks_lost);
  process::metrics::add(tasks_gone);

  process::metrics::add(executors_registering);
  process::metrics::add(executors_running);
  process::metrics::add(executors_terminating);
  process::metrics::add(executors_terminated);
  process::metrics::add(executors_preempted);

  process::metrics::add(valid_status_updates);
  process::metrics::add(invalid_status_updates);

  process::metrics::add(valid_framework_messages);
  process::metrics::add(invalid_framework_messages);

  process::metrics::add(executor_directory_max_allowed_age_secs);

  process::metrics::add(container_launch_errors);

  // Create resource gauges.
  // TODO(dhamon): Set these up dynamically when creating a slave
  // based on the resources it exposes.
  const string resources[] = {"cpus", "gpus", "mem", "disk"};

  foreach (const string& resource, resources) {
    PullGauge total(
        "slave/" + resource + "_total",
        defer(slave, &Slave::_resources_total, resource));

    PullGauge used(
        "slave/" + resource + "_used",
        defer(slave, &Slave::_resources_used, resource));

    PullGauge percent(
        "slave/" + resource + "_percent",
        defer(slave, &Slave::_resources_percent, resource));

    resources_total.push_back(total);
    resources_used.push_back(used);
    resources_percent.push_back(percent);

    process::metrics::add(total);
    process::metrics::add(used);
    process::metrics::add(percent);
  }

  foreach (const string& resource, resources) {
    PullGauge total(
        "slave/" + resource + "_revocable_total",
        defer(slave, &Slave::_resources_revocable_total, resource));

    PullGauge used(
        "slave/" + resource + "_revocable_used",
        defer(slave, &Slave::_resources_revocable_used, resource));

    PullGauge percent(
        "slave/" + resource + "_revocable_percent",
        defer(slave, &Slave::_resources_revocable_percent, resource));

    resources_revocable_total.push_back(total);
    resources_revocable_used.push_back(used);
    resources_revocable_percent.push_back(percent);

    process::metrics::add(total);
    process::metrics::add(used);
    process::metrics::add(percent);
  }
}


Metrics::~Metrics()
{
  // TODO(dhamon): Check return values of unregistered metrics.
  process::metrics::remove(uptime_secs);
  process::metrics::remove(registered);

  process::metrics::remove(recovery_errors);

  process::metrics::remove(frameworks_active);

  process::metrics::remove(tasks_staging);
  process::metrics::remove(tasks_starting);
  process::metrics::remove(tasks_running);
  process::metrics::remove(tasks_killing);
  process::metrics::remove(tasks_finished);
  process::metrics::remove(tasks_failed);
  process::metrics::remove(tasks_killed);
  process::metrics::remove(tasks_lost);
  process::metrics::remove(tasks_gone);

  process::metrics::remove(executors_registering);
  process::metrics::remove(executors_running);
  process::metrics::remove(executors_terminating);
  process::metrics::remove(executors_terminated);
  process::metrics::remove(executors_preempted);

  process::metrics::remove(valid_status_updates);
  process::metrics::remove(invalid_status_updates);

  process::metrics::remove(valid_framework_messages);
  process::metrics::remove(invalid_framework_messages);

  process::metrics::remove(executor_directory_max_allowed_age_secs);

  process::metrics::remove(container_launch_errors);

  foreach (const PullGauge& gauge, resources_total) {
    process::metrics::remove(gauge);
  }
  resources_total.clear();

  foreach (const PullGauge& gauge, resources_used) {
    process::metrics::remove(gauge);
  }
  resources_used.clear();

  foreach (const PullGauge& gauge, resources_percent) {
    process::metrics::remove(gauge);
  }
  resources_percent.clear();

  foreach (const PullGauge& gauge, resources_revocable_total) {
    process::metrics::remove(gauge);
  }
  resources_revocable_total.clear();

  foreach (const PullGauge& gauge, resources_revocable_used) {
    process::metrics::remove(gauge);
  }
  resources_revocable_used.clear();

  foreach (const PullGauge& gauge, resources_revocable_percent) {
    process::metrics::remove(gauge);
  }
  resources_revocable_percent.clear();

  if (recovery_time_secs.isSome()) {
    process::metrics::remove(recovery_time_secs.get());
  }
}


void Metrics::setRecoveryTime(const Duration& duration)
{
  CHECK_NONE(recovery_time_secs);

  const double recovery_seconds = duration.secs();

  recovery_time_secs = process::metrics::PullGauge(
        "slave/recovery_time_secs",
        [recovery_seconds]() { return recovery_seconds;});

  process::metrics::add(recovery_time_secs.get());
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
