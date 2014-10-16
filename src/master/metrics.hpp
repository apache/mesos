/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __MASTER_METRICS_HPP__
#define __MASTER_METRICS_HPP__

#include <string>
#include <vector>

#include <process/metrics/counter.hpp>
#include <process/metrics/gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/hashmap.hpp>

#include "mesos/mesos.hpp"

namespace mesos {
namespace master {

class Master;

struct Metrics
{
  explicit Metrics(const Master& master);

  ~Metrics();

  process::metrics::Gauge uptime_secs;
  process::metrics::Gauge elected;

  process::metrics::Gauge slaves_connected;
  process::metrics::Gauge slaves_disconnected;
  process::metrics::Gauge slaves_active;
  process::metrics::Gauge slaves_inactive;

  process::metrics::Gauge frameworks_connected;
  process::metrics::Gauge frameworks_disconnected;
  process::metrics::Gauge frameworks_active;
  process::metrics::Gauge frameworks_inactive;

  process::metrics::Gauge outstanding_offers;

  // Task state metrics.
  process::metrics::Gauge tasks_staging;
  process::metrics::Gauge tasks_starting;
  process::metrics::Gauge tasks_running;
  process::metrics::Counter tasks_finished;
  process::metrics::Counter tasks_failed;
  process::metrics::Counter tasks_killed;
  process::metrics::Counter tasks_lost;
  process::metrics::Counter tasks_error;

  typedef hashmap<TaskStatus::Reason, process::metrics::Counter> Reasons;
  typedef hashmap<TaskStatus::Source, Reasons> SourcesReasons;

  // NOTE: We only track metrics sources and reasons for terminal states.
  hashmap<TaskState, SourcesReasons> tasks_states;

  // Message counters.
  process::metrics::Counter dropped_messages;

  // Metrics specific to frameworks of a common principal.
  // These metrics have names prefixed by "frameworks/<principal>/".
  struct Frameworks
  {
    // Counters for messages from all frameworks of this principal.
    // Note: We only count messages from active scheduler
    // *instances* while they are *registered*. i.e., messages
    // prior to the completion of (re)registration
    // (AuthenticateMessage and (Re)RegisterFrameworkMessage) and
    // messages from an inactive scheduler instance (after the
    // framework has failed over) are not counted.

    // Framework messages received (before processing).
    process::metrics::Counter messages_received;

    // Framework messages processed.
    // NOTE: This doesn't include dropped messages. Processing of
    // a message may be throttled by a RateLimiter if one is
    // configured for this principal. Also due to Master's
    // asynchronous nature, this doesn't necessarily mean the work
    // requested by this message has finished.
    process::metrics::Counter messages_processed;

    explicit Frameworks(const std::string& principal)
      : messages_received("frameworks/" + principal + "/messages_received"),
        messages_processed("frameworks/" + principal + "/messages_processed")
    {
      process::metrics::add(messages_received);
      process::metrics::add(messages_processed);
    }

    ~Frameworks()
    {
      process::metrics::remove(messages_received);
      process::metrics::remove(messages_processed);
    }
  };

  // Per-framework-principal metrics keyed by the framework
  // principal.
  hashmap<std::string, process::Owned<Frameworks>> frameworks;

  // Messages from schedulers.
  process::metrics::Counter messages_register_framework;
  process::metrics::Counter messages_reregister_framework;
  process::metrics::Counter messages_unregister_framework;
  process::metrics::Counter messages_deactivate_framework;
  process::metrics::Counter messages_kill_task;
  process::metrics::Counter messages_status_update_acknowledgement;
  process::metrics::Counter messages_resource_request;
  process::metrics::Counter messages_launch_tasks;
  process::metrics::Counter messages_decline_offers;
  process::metrics::Counter messages_revive_offers;
  process::metrics::Counter messages_reconcile_tasks;
  process::metrics::Counter messages_framework_to_executor;

  // Messages from slaves.
  process::metrics::Counter messages_register_slave;
  process::metrics::Counter messages_reregister_slave;
  process::metrics::Counter messages_unregister_slave;
  process::metrics::Counter messages_status_update;
  process::metrics::Counter messages_exited_executor;

  // Messages from both schedulers and slaves.
  process::metrics::Counter messages_authenticate;

  process::metrics::Counter valid_framework_to_executor_messages;
  process::metrics::Counter invalid_framework_to_executor_messages;

  process::metrics::Counter valid_status_updates;
  process::metrics::Counter invalid_status_updates;

  process::metrics::Counter valid_status_update_acknowledgements;
  process::metrics::Counter invalid_status_update_acknowledgements;

  // Recovery counters.
  process::metrics::Counter recovery_slave_removals;

  // Process metrics.
  process::metrics::Gauge event_queue_messages;
  process::metrics::Gauge event_queue_dispatches;
  process::metrics::Gauge event_queue_http_requests;

  // Successful registry operations.
  process::metrics::Counter slave_registrations;
  process::metrics::Counter slave_reregistrations;
  process::metrics::Counter slave_removals;

  // Slave observer metrics.
  process::metrics::Counter slave_shutdowns_scheduled;
  process::metrics::Counter slave_shutdowns_canceled;

  // Resource metrics.
  std::vector<process::metrics::Gauge> resources_total;
  std::vector<process::metrics::Gauge> resources_used;
  std::vector<process::metrics::Gauge> resources_percent;

  void incrementTasksStates(
      TaskState state, TaskStatus::Source source, TaskStatus::Reason reason);
};

} // namespace master {
} // namespace mesos {

#endif // __MASTER_METRICS_HPP__
