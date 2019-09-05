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

#ifndef __MASTER_METRICS_HPP__
#define __MASTER_METRICS_HPP__

#include <string>
#include <vector>

#include <mesos/scheduler/scheduler.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/push_gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/hashmap.hpp>

#include "mesos/mesos.hpp"
#include "mesos/type_utils.hpp"

namespace mesos {
namespace internal {
namespace master {

class Master;

struct Metrics
{
  explicit Metrics(const Master& master);

  ~Metrics();

  process::metrics::PullGauge uptime_secs;
  process::metrics::PullGauge elected;

  process::metrics::PullGauge slaves_connected;
  process::metrics::PullGauge slaves_disconnected;
  process::metrics::PullGauge slaves_active;
  process::metrics::PullGauge slaves_inactive;
  process::metrics::PullGauge slaves_unreachable;

  process::metrics::PullGauge frameworks_connected;
  process::metrics::PullGauge frameworks_disconnected;
  process::metrics::PullGauge frameworks_active;
  process::metrics::PullGauge frameworks_inactive;

  process::metrics::PullGauge outstanding_offers;

  // Contains counters 'prefix/pending', 'prefix/recovering', etc.
  struct OperationStates {
    OperationStates(const std::string& prefix);
    ~OperationStates();

    void update(const OperationState& state, int delta);

    process::metrics::Counter total;

    process::metrics::PushGauge pending;
    process::metrics::PushGauge recovering;
    process::metrics::PushGauge unreachable;
    process::metrics::Counter finished;
    process::metrics::Counter failed;
    process::metrics::Counter error;
    process::metrics::Counter dropped;
    process::metrics::Counter gone_by_operator;
  };

  // Operation states are tracked in two granularities: master-wide and
  // per operation type. Additionally, for every framework the types of
  // operations are tracked but not their states.
  //
  // NOTE: These metrics are missing the implicit operation statuses that
  // are generated on operation reconciliation. For example, when a framework
  // queries the state of an unknown operation on an unreachable agent,
  // the master will generate an `OPERATION_UNREACHABLE` update that is not
  // counted by these metrics.
  OperationStates operation_states;
  hashmap<Offer::Operation::Type, OperationStates> operation_type_states;

  void incrementOperationState(
      Offer::Operation::Type type,
      const OperationState& state);

  void decrementOperationState(
      Offer::Operation::Type type,
      const OperationState& state);

  void transitionOperationState(
      Offer::Operation::Type type,
      const OperationState& oldState,
      const OperationState& newState);

  process::metrics::PushGauge operator_event_stream_subscribers;

  // Task state metrics.
  process::metrics::PullGauge tasks_staging;
  process::metrics::PullGauge tasks_starting;
  process::metrics::PullGauge tasks_running;
  process::metrics::PullGauge tasks_unreachable;
  process::metrics::PullGauge tasks_killing;
  process::metrics::Counter tasks_finished;
  process::metrics::Counter tasks_failed;
  process::metrics::Counter tasks_killed;
  process::metrics::Counter tasks_lost;
  process::metrics::Counter tasks_error;
  process::metrics::Counter tasks_dropped;
  process::metrics::Counter tasks_gone;
  process::metrics::Counter tasks_gone_by_operator;

  typedef hashmap<TaskStatus::Reason, process::metrics::Counter> Reasons;
  typedef hashmap<TaskStatus::Source, Reasons> SourcesReasons;

  // NOTE: We only track metrics sources and reasons for terminal states.
  hashmap<TaskState, SourcesReasons> tasks_states;

  // Message counters.
  process::metrics::Counter dropped_messages;

  // HTTP cache hits.
  // TODO(bevers): Collect these per endpoint once per-endpoint
  // metrics get merged.
  process::metrics::Counter http_cache_hits;

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
  process::metrics::Counter messages_suppress_offers;
  process::metrics::Counter messages_reconcile_operations;
  process::metrics::Counter messages_reconcile_tasks;
  process::metrics::Counter messages_framework_to_executor;
  process::metrics::Counter messages_operation_status_update_acknowledgement;

  // Messages from executors.
  process::metrics::Counter messages_executor_to_framework;

  // Messages from slaves.
  process::metrics::Counter messages_register_slave;
  process::metrics::Counter messages_reregister_slave;
  process::metrics::Counter messages_unregister_slave;
  process::metrics::Counter messages_status_update;
  process::metrics::Counter messages_operation_status_update;
  process::metrics::Counter messages_exited_executor;
  process::metrics::Counter messages_update_slave;

  // Messages from both schedulers and slaves.
  process::metrics::Counter messages_authenticate;

  process::metrics::Counter valid_framework_to_executor_messages;
  process::metrics::Counter invalid_framework_to_executor_messages;
  process::metrics::Counter valid_executor_to_framework_messages;
  process::metrics::Counter invalid_executor_to_framework_messages;

  process::metrics::Counter valid_status_updates;
  process::metrics::Counter invalid_status_updates;

  process::metrics::Counter valid_status_update_acknowledgements;
  process::metrics::Counter invalid_status_update_acknowledgements;

  process::metrics::Counter valid_operation_status_updates;
  process::metrics::Counter invalid_operation_status_updates;

  process::metrics::Counter valid_operation_status_update_acknowledgements;
  process::metrics::Counter invalid_operation_status_update_acknowledgements;

  // Recovery counters.
  process::metrics::Counter recovery_slave_removals;

  // Process metrics.
  process::metrics::PullGauge event_queue_messages;
  process::metrics::PullGauge event_queue_dispatches;
  process::metrics::PullGauge event_queue_http_requests;

  // Successful registry operations.
  process::metrics::Counter slave_registrations;
  process::metrics::Counter slave_reregistrations;
  process::metrics::Counter slave_removals;
  process::metrics::Counter slave_removals_reason_unhealthy;
  process::metrics::Counter slave_removals_reason_unregistered;
  process::metrics::Counter slave_removals_reason_registered;

  // Slave observer metrics.
  //
  // TODO(neilc): The `slave_shutdowns_xxx` metrics are deprecated and
  // will always be zero. Remove in Mesos 2.0.
  process::metrics::Counter slave_shutdowns_scheduled;
  process::metrics::Counter slave_shutdowns_completed;
  process::metrics::Counter slave_shutdowns_canceled;

  process::metrics::Counter slave_unreachable_scheduled;
  process::metrics::Counter slave_unreachable_completed;
  process::metrics::Counter slave_unreachable_canceled;

  // Non-revocable resources.
  std::vector<process::metrics::PullGauge> resources_total;
  std::vector<process::metrics::PullGauge> resources_used;
  std::vector<process::metrics::PullGauge> resources_percent;

  // Revocable resources.
  std::vector<process::metrics::PullGauge> resources_revocable_total;
  std::vector<process::metrics::PullGauge> resources_revocable_used;
  std::vector<process::metrics::PullGauge> resources_revocable_percent;

  void incrementInvalidSchedulerCalls(const mesos::scheduler::Call& call);

  void incrementTasksStates(
      const TaskState& state,
      const TaskStatus::Source& source,
      const TaskStatus::Reason& reason);
};


struct FrameworkMetrics
{
  FrameworkMetrics(
      const FrameworkInfo& _frameworkInfo,
      bool publishPerFrameworkMetrics);

  ~FrameworkMetrics();

  void incrementCall(const mesos::scheduler::Call::Type& callType);

  void incrementEvent(const mesos::scheduler::Event& event);

  // Overloads to convert unversioned messages into events.
  void incrementEvent(const FrameworkErrorMessage& message);
  void incrementEvent(const ExitedExecutorMessage& message);
  void incrementEvent(const LostSlaveMessage& message);
  void incrementEvent(const InverseOffersMessage& message);
  void incrementEvent(const ExecutorToFrameworkMessage& message);
  void incrementEvent(const ResourceOffersMessage& message);
  void incrementEvent(const RescindResourceOfferMessage& message);
  void incrementEvent(const RescindInverseOfferMessage& message);
  void incrementEvent(const FrameworkRegisteredMessage& message);
  void incrementEvent(const FrameworkReregisteredMessage& message);
  void incrementEvent(const StatusUpdateMessage& message);
  void incrementEvent(const UpdateOperationStatusMessage& message);

  void incrementTaskState(const TaskState& state);
  void decrementActiveTaskState(const TaskState& state);

  void incrementOperation(const Offer::Operation& operation);

  template <typename T> void addMetric(const T& metric);
  template <typename T> void removeMetric(const T& metric);

  const std::string metricPrefix;

  bool publishPerFrameworkMetrics;

  process::metrics::PushGauge subscribed;

  process::metrics::Counter calls;
  hashmap<mesos::scheduler::Call::Type, process::metrics::Counter> call_types;

  process::metrics::Counter events;
  hashmap<mesos::scheduler::Event::Type, process::metrics::Counter> event_types;

  process::metrics::Counter offers_sent;
  process::metrics::Counter offers_accepted;
  process::metrics::Counter offers_declined;
  process::metrics::Counter offers_rescinded;

  hashmap<TaskState, process::metrics::Counter> terminal_task_states;

  hashmap<TaskState, process::metrics::PushGauge> active_task_states;

  process::metrics::Counter operations;
  hashmap<Offer::Operation::Type, process::metrics::Counter> operation_types;
};


std::string getFrameworkMetricPrefix(const FrameworkInfo& frameworkInfo);

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_METRICS_HPP__
