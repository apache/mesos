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

#include <mesos/scheduler/scheduler.hpp>

#include <process/http.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/push_gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/foreach.hpp>

#include "master/master.hpp"
#include "master/metrics.hpp"

using process::metrics::Counter;
using process::metrics::PullGauge;
using process::metrics::PushGauge;

using std::string;

namespace mesos {
namespace internal {
namespace master {

// Message counters are named with "messages_" prefix so they can
// be grouped together alphabetically in the output.
// TODO(alexandra.sava): Add metrics for registered and removed slaves.
Metrics::Metrics(const Master& master)
  : uptime_secs(
        "master/uptime_secs",
        defer(master, &Master::_uptime_secs)),
    elected(
        "master/elected",
        defer(master, &Master::_elected)),
    slaves_connected(
        "master/slaves_connected",
        defer(master, &Master::_slaves_connected)),
    slaves_disconnected(
        "master/slaves_disconnected",
        defer(master, &Master::_slaves_disconnected)),
    slaves_active(
        "master/slaves_active",
        defer(master, &Master::_slaves_active)),
    slaves_inactive(
        "master/slaves_inactive",
        defer(master, &Master::_slaves_inactive)),
    slaves_unreachable(
        "master/slaves_unreachable",
        defer(master, &Master::_slaves_unreachable)),
    frameworks_connected(
        "master/frameworks_connected",
        defer(master, &Master::_frameworks_connected)),
    frameworks_disconnected(
        "master/frameworks_disconnected",
        defer(master, &Master::_frameworks_disconnected)),
    frameworks_active(
        "master/frameworks_active",
        defer(master, &Master::_frameworks_active)),
    frameworks_inactive(
        "master/frameworks_inactive",
        defer(master, &Master::_frameworks_inactive)),
    outstanding_offers(
        "master/outstanding_offers",
        defer(master, &Master::_outstanding_offers)),
    operation_states(
        "master/operations/"),
    operator_event_stream_subscribers(
        "master/operator_event_stream_subscribers"),
    tasks_staging(
        "master/tasks_staging",
        defer(master, &Master::_tasks_staging)),
    tasks_starting(
        "master/tasks_starting",
        defer(master, &Master::_tasks_starting)),
    tasks_running(
        "master/tasks_running",
        defer(master, &Master::_tasks_running)),
    tasks_unreachable(
        "master/tasks_unreachable",
        defer(master, &Master::_tasks_unreachable)),
    tasks_killing(
        "master/tasks_killing",
        defer(master, &Master::_tasks_killing)),
    tasks_finished(
        "master/tasks_finished"),
    tasks_failed(
        "master/tasks_failed"),
    tasks_killed(
        "master/tasks_killed"),
    tasks_lost(
        "master/tasks_lost"),
    tasks_error(
        "master/tasks_error"),
    tasks_dropped(
        "master/tasks_dropped"),
    tasks_gone(
        "master/tasks_gone"),
    tasks_gone_by_operator(
        "master/tasks_gone_by_operator"),
    dropped_messages(
        "master/dropped_messages"),
    http_cache_hits(
        "master/http_cache_hits"),
    messages_register_framework(
        "master/messages_register_framework"),
    messages_reregister_framework(
        "master/messages_reregister_framework"),
    messages_unregister_framework(
        "master/messages_unregister_framework"),
    messages_deactivate_framework(
        "master/messages_deactivate_framework"),
    messages_kill_task(
        "master/messages_kill_task"),
    messages_status_update_acknowledgement(
        "master/messages_status_update_acknowledgement"),
    messages_resource_request(
        "master/messages_resource_request"),
    messages_launch_tasks(
        "master/messages_launch_tasks"),
    messages_decline_offers(
        "master/messages_decline_offers"),
    messages_revive_offers(
        "master/messages_revive_offers"),
    messages_suppress_offers(
        "master/messages_suppress_offers"),
    messages_reconcile_operations(
        "master/messages_reconcile_operations"),
    messages_reconcile_tasks(
        "master/messages_reconcile_tasks"),
    messages_framework_to_executor(
        "master/messages_framework_to_executor"),
    messages_operation_status_update_acknowledgement(
        "master/messages_operation_status_update_acknowledgement"),
    messages_executor_to_framework(
        "master/messages_executor_to_framework"),
    messages_register_slave(
        "master/messages_register_slave"),
    messages_reregister_slave(
        "master/messages_reregister_slave"),
    messages_unregister_slave(
        "master/messages_unregister_slave"),
    messages_status_update(
        "master/messages_status_update"),
    messages_operation_status_update(
        "master/messages_operation_status_update"),
    messages_exited_executor(
        "master/messages_exited_executor"),
    messages_update_slave(
        "master/messages_update_slave"),
    messages_authenticate(
        "master/messages_authenticate"),
    valid_framework_to_executor_messages(
        "master/valid_framework_to_executor_messages"),
    invalid_framework_to_executor_messages(
        "master/invalid_framework_to_executor_messages"),
    valid_executor_to_framework_messages(
        "master/valid_executor_to_framework_messages"),
    invalid_executor_to_framework_messages(
        "master/invalid_executor_to_framework_messages"),
    valid_status_updates(
        "master/valid_status_updates"),
    invalid_status_updates(
        "master/invalid_status_updates"),
    valid_status_update_acknowledgements(
        "master/valid_status_update_acknowledgements"),
    invalid_status_update_acknowledgements(
        "master/invalid_status_update_acknowledgements"),
    valid_operation_status_updates(
        "master/valid_operation_status_updates"),
    invalid_operation_status_updates(
        "master/invalid_operation_status_updates"),
    valid_operation_status_update_acknowledgements(
        "master/valid_operation_status_update_acknowledgements"),
    invalid_operation_status_update_acknowledgements(
        "master/invalid_operation_status_update_acknowledgements"),
    recovery_slave_removals(
        "master/recovery_slave_removals"),
    event_queue_messages(
        "master/event_queue_messages",
        defer(master, &Master::_event_queue_messages)),
    event_queue_dispatches(
        "master/event_queue_dispatches",
        defer(master, &Master::_event_queue_dispatches)),
    event_queue_http_requests(
        "master/event_queue_http_requests",
        defer(master, &Master::_event_queue_http_requests)),
    slave_registrations(
        "master/slave_registrations"),
    slave_reregistrations(
        "master/slave_reregistrations"),
    slave_removals(
        "master/slave_removals"),
    slave_removals_reason_unhealthy(
        "master/slave_removals/reason_unhealthy"),
    slave_removals_reason_unregistered(
        "master/slave_removals/reason_unregistered"),
    slave_removals_reason_registered(
        "master/slave_removals/reason_registered"),
    slave_shutdowns_scheduled(
        "master/slave_shutdowns_scheduled"),
    slave_shutdowns_completed(
        "master/slave_shutdowns_completed"),
    slave_shutdowns_canceled(
        "master/slave_shutdowns_canceled"),
    slave_unreachable_scheduled(
        "master/slave_unreachable_scheduled"),
    slave_unreachable_completed(
        "master/slave_unreachable_completed"),
    slave_unreachable_canceled(
        "master/slave_unreachable_canceled")
{
  // TODO(dhamon): Check return values of 'add'.
  process::metrics::add(uptime_secs);
  process::metrics::add(elected);

  process::metrics::add(slaves_connected);
  process::metrics::add(slaves_disconnected);
  process::metrics::add(slaves_active);
  process::metrics::add(slaves_inactive);
  process::metrics::add(slaves_unreachable);

  process::metrics::add(frameworks_connected);
  process::metrics::add(frameworks_disconnected);
  process::metrics::add(frameworks_active);
  process::metrics::add(frameworks_inactive);

  process::metrics::add(outstanding_offers);

  process::metrics::add(operator_event_stream_subscribers);

  process::metrics::add(tasks_staging);
  process::metrics::add(tasks_starting);
  process::metrics::add(tasks_running);
  process::metrics::add(tasks_killing);
  process::metrics::add(tasks_finished);
  process::metrics::add(tasks_failed);
  process::metrics::add(tasks_killed);
  process::metrics::add(tasks_lost);
  process::metrics::add(tasks_error);
  process::metrics::add(tasks_dropped);
  process::metrics::add(tasks_unreachable);
  process::metrics::add(tasks_gone);
  process::metrics::add(tasks_gone_by_operator);

  process::metrics::add(dropped_messages);
  process::metrics::add(http_cache_hits);

  // Messages from schedulers.
  process::metrics::add(messages_register_framework);
  process::metrics::add(messages_reregister_framework);
  process::metrics::add(messages_unregister_framework);
  process::metrics::add(messages_deactivate_framework);
  process::metrics::add(messages_kill_task);
  process::metrics::add(messages_status_update_acknowledgement);
  process::metrics::add(messages_operation_status_update_acknowledgement);
  process::metrics::add(messages_resource_request);
  process::metrics::add(messages_launch_tasks);
  process::metrics::add(messages_decline_offers);
  process::metrics::add(messages_revive_offers);
  process::metrics::add(messages_suppress_offers);
  process::metrics::add(messages_reconcile_operations);
  process::metrics::add(messages_reconcile_tasks);
  process::metrics::add(messages_framework_to_executor);
  process::metrics::add(messages_executor_to_framework);

  // Messages from slaves.
  process::metrics::add(messages_register_slave);
  process::metrics::add(messages_reregister_slave);
  process::metrics::add(messages_unregister_slave);
  process::metrics::add(messages_status_update);
  process::metrics::add(messages_operation_status_update);
  process::metrics::add(messages_exited_executor);
  process::metrics::add(messages_update_slave);

  // Messages from both schedulers and slaves.
  process::metrics::add(messages_authenticate);

  process::metrics::add(valid_framework_to_executor_messages);
  process::metrics::add(invalid_framework_to_executor_messages);

  process::metrics::add(valid_executor_to_framework_messages);
  process::metrics::add(invalid_executor_to_framework_messages);

  process::metrics::add(valid_status_updates);
  process::metrics::add(invalid_status_updates);

  process::metrics::add(valid_status_update_acknowledgements);
  process::metrics::add(invalid_status_update_acknowledgements);

  process::metrics::add(valid_operation_status_updates);
  process::metrics::add(invalid_operation_status_updates);

  process::metrics::add(valid_operation_status_update_acknowledgements);
  process::metrics::add(invalid_operation_status_update_acknowledgements);

  process::metrics::add(recovery_slave_removals);

  process::metrics::add(event_queue_messages);
  process::metrics::add(event_queue_dispatches);
  process::metrics::add(event_queue_http_requests);

  process::metrics::add(slave_registrations);
  process::metrics::add(slave_reregistrations);
  process::metrics::add(slave_removals);
  process::metrics::add(slave_removals_reason_unhealthy);
  process::metrics::add(slave_removals_reason_unregistered);
  process::metrics::add(slave_removals_reason_registered);

  process::metrics::add(slave_shutdowns_scheduled);
  process::metrics::add(slave_shutdowns_completed);
  process::metrics::add(slave_shutdowns_canceled);

  process::metrics::add(slave_unreachable_scheduled);
  process::metrics::add(slave_unreachable_completed);
  process::metrics::add(slave_unreachable_canceled);

  // Create resource gauges.
  // TODO(dhamon): Set these up dynamically when adding a slave based on the
  // resources the slave exposes.
  const string resources[] = {"cpus", "gpus", "mem", "disk"};

  foreach (const string& resource, resources) {
    PullGauge total(
        "master/" + resource + "_total",
        defer(master, &Master::_resources_total, resource));

    PullGauge used(
        "master/" + resource + "_used",
        defer(master, &Master::_resources_used, resource));

    PullGauge percent(
        "master/" + resource + "_percent",
        defer(master, &Master::_resources_percent, resource));

    resources_total.push_back(total);
    resources_used.push_back(used);
    resources_percent.push_back(percent);

    process::metrics::add(total);
    process::metrics::add(used);
    process::metrics::add(percent);
  }

  foreach (const string& resource, resources) {
    PullGauge total(
        "master/" + resource + "_revocable_total",
        defer(master, &Master::_resources_revocable_total, resource));

    PullGauge used(
        "master/" + resource + "_revocable_used",
        defer(master, &Master::_resources_revocable_used, resource));

    PullGauge percent(
        "master/" + resource + "_revocable_percent",
        defer(master, &Master::_resources_revocable_percent, resource));

    resources_revocable_total.push_back(total);
    resources_revocable_used.push_back(used);
    resources_revocable_percent.push_back(percent);

    process::metrics::add(total);
    process::metrics::add(used);
    process::metrics::add(percent);
  }

  for (int index = 0;
       index < Offer::Operation::Type_descriptor()->value_count();
       index++) {
    const google::protobuf::EnumValueDescriptor* descriptor =
      Offer::Operation::Type_descriptor()->value(index);

    const Offer::Operation::Type type =
      static_cast<Offer::Operation::Type>(descriptor->number());

    // Skip `LAUNCH` and `LAUNCH_GROUP` because they are special-cased
    // in the master and don't go through the usual offer operation feedback
    // cycle.
    if (type == Offer::Operation::UNKNOWN ||
        type == Offer::Operation::LAUNCH ||
        type == Offer::Operation::LAUNCH_GROUP) {
      continue;
    }

    std::string prefix =
      "master/operations/" + strings::lower(descriptor->name()) + "/";

    operation_type_states.emplace(type, prefix);
  }
}


Metrics::~Metrics()
{
  // TODO(dhamon): Check return values of 'remove'.
  process::metrics::remove(uptime_secs);
  process::metrics::remove(elected);

  process::metrics::remove(slaves_connected);
  process::metrics::remove(slaves_disconnected);
  process::metrics::remove(slaves_active);
  process::metrics::remove(slaves_inactive);
  process::metrics::remove(slaves_unreachable);

  process::metrics::remove(frameworks_connected);
  process::metrics::remove(frameworks_disconnected);
  process::metrics::remove(frameworks_active);
  process::metrics::remove(frameworks_inactive);

  process::metrics::remove(outstanding_offers);

  process::metrics::remove(operator_event_stream_subscribers);

  process::metrics::remove(tasks_staging);
  process::metrics::remove(tasks_starting);
  process::metrics::remove(tasks_running);
  process::metrics::remove(tasks_killing);
  process::metrics::remove(tasks_finished);
  process::metrics::remove(tasks_failed);
  process::metrics::remove(tasks_killed);
  process::metrics::remove(tasks_lost);
  process::metrics::remove(tasks_error);
  process::metrics::remove(tasks_dropped);
  process::metrics::remove(tasks_unreachable);
  process::metrics::remove(tasks_gone);
  process::metrics::remove(tasks_gone_by_operator);

  process::metrics::remove(dropped_messages);
  process::metrics::remove(http_cache_hits);

  // Messages from schedulers.
  process::metrics::remove(messages_register_framework);
  process::metrics::remove(messages_reregister_framework);
  process::metrics::remove(messages_unregister_framework);
  process::metrics::remove(messages_deactivate_framework);
  process::metrics::remove(messages_kill_task);
  process::metrics::remove(messages_status_update_acknowledgement);
  process::metrics::remove(messages_operation_status_update_acknowledgement);
  process::metrics::remove(messages_resource_request);
  process::metrics::remove(messages_launch_tasks);
  process::metrics::remove(messages_decline_offers);
  process::metrics::remove(messages_revive_offers);
  process::metrics::remove(messages_suppress_offers);
  process::metrics::remove(messages_reconcile_operations);
  process::metrics::remove(messages_reconcile_tasks);
  process::metrics::remove(messages_framework_to_executor);
  process::metrics::remove(messages_executor_to_framework);

  // Messages from slaves.
  process::metrics::remove(messages_register_slave);
  process::metrics::remove(messages_reregister_slave);
  process::metrics::remove(messages_unregister_slave);
  process::metrics::remove(messages_status_update);
  process::metrics::remove(messages_operation_status_update);
  process::metrics::remove(messages_exited_executor);
  process::metrics::remove(messages_update_slave);

  // Messages from both schedulers and slaves.
  process::metrics::remove(messages_authenticate);

  process::metrics::remove(valid_framework_to_executor_messages);
  process::metrics::remove(invalid_framework_to_executor_messages);

  process::metrics::remove(valid_executor_to_framework_messages);
  process::metrics::remove(invalid_executor_to_framework_messages);

  process::metrics::remove(valid_status_updates);
  process::metrics::remove(invalid_status_updates);

  process::metrics::remove(valid_status_update_acknowledgements);
  process::metrics::remove(invalid_status_update_acknowledgements);

  process::metrics::remove(valid_operation_status_updates);
  process::metrics::remove(invalid_operation_status_updates);

  process::metrics::remove(valid_operation_status_update_acknowledgements);
  process::metrics::remove(invalid_operation_status_update_acknowledgements);

  process::metrics::remove(recovery_slave_removals);

  process::metrics::remove(event_queue_messages);
  process::metrics::remove(event_queue_dispatches);
  process::metrics::remove(event_queue_http_requests);

  process::metrics::remove(slave_registrations);
  process::metrics::remove(slave_reregistrations);
  process::metrics::remove(slave_removals);
  process::metrics::remove(slave_removals_reason_unhealthy);
  process::metrics::remove(slave_removals_reason_unregistered);
  process::metrics::remove(slave_removals_reason_registered);

  process::metrics::remove(slave_shutdowns_scheduled);
  process::metrics::remove(slave_shutdowns_completed);
  process::metrics::remove(slave_shutdowns_canceled);

  process::metrics::remove(slave_unreachable_scheduled);
  process::metrics::remove(slave_unreachable_completed);
  process::metrics::remove(slave_unreachable_canceled);

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

  foreachvalue (const auto& source_reason, tasks_states) {
    foreachvalue (const auto& reason_counter, source_reason) {
      foreachvalue (const Counter& counter, reason_counter) {
        process::metrics::remove(counter);
      }
    }
  }
  tasks_states.clear();
}


void Metrics::incrementInvalidSchedulerCalls(const scheduler::Call& call) {
  if (call.type() == scheduler::Call::ACKNOWLEDGE) {
    invalid_status_update_acknowledgements++;
  }

  if (call.type() == scheduler::Call::ACKNOWLEDGE_OPERATION_STATUS) {
    invalid_operation_status_update_acknowledgements++;
  }

  if (call.type() == scheduler::Call::MESSAGE) {
    invalid_framework_to_executor_messages++;
  }

  // TODO(gkleiman): Increment other metrics when we add counters for all
  // the different types of scheduler calls. See MESOS-8533.
}


void Metrics::incrementTasksStates(
    const TaskState& state,
    const TaskStatus::Source& source,
    const TaskStatus::Reason& reason)
{
  if (!tasks_states.contains(state)) {
    tasks_states[state] = SourcesReasons();
  }

  if (!tasks_states[state].contains(source)) {
    tasks_states[state][source] = Reasons();
  }

  if (!tasks_states[state][source].contains(reason)) {
    Counter counter = Counter(
        "master/" +
        strings::lower(TaskState_Name(state)) + "/" +
        strings::lower(TaskStatus::Source_Name(source)) + "/" +
        strings::lower(TaskStatus::Reason_Name(reason)));

    tasks_states[state][source].put(reason, counter);
    process::metrics::add(counter);
  }

  Counter counter = tasks_states[state][source].get(reason).get();
  counter++;
}


Metrics::OperationStates::OperationStates(const std::string& prefix)
  : total(prefix + "total"),
    pending(prefix + "pending"),
    recovering(prefix + "recovering"),
    unreachable(prefix + "unreachable"),
    finished(prefix + "finished"),
    failed(prefix + "failed"),
    error(prefix + "error"),
    dropped(prefix + "dropped"),
    gone_by_operator(prefix + "gone_by_operator")
{
  process::metrics::add(total);
  process::metrics::add(pending);
  process::metrics::add(recovering);
  process::metrics::add(finished);
  process::metrics::add(failed);
  process::metrics::add(error);
  process::metrics::add(dropped);
  process::metrics::add(unreachable);
  process::metrics::add(gone_by_operator);
}


Metrics::OperationStates::~OperationStates()
{
  process::metrics::remove(total);
  process::metrics::remove(pending);
  process::metrics::remove(recovering);
  process::metrics::remove(finished);
  process::metrics::remove(failed);
  process::metrics::remove(error);
  process::metrics::remove(dropped);
  process::metrics::remove(unreachable);
  process::metrics::remove(gone_by_operator);
}


void Metrics::OperationStates::update(
    const OperationState& operationState,
    int delta)
{
  total += delta;

  switch(operationState) {
    case OPERATION_FINISHED:
      finished += delta;
      break;
    case OPERATION_DROPPED:
      dropped += delta;
      break;
    case OPERATION_ERROR:
      error += delta;
      break;
    case OPERATION_FAILED:
      failed += delta;
      break;
    case OPERATION_GONE_BY_OPERATOR:
      gone_by_operator += delta;
      break;
    case OPERATION_PENDING:
      pending += delta;
      break;
    case OPERATION_UNREACHABLE:
      unreachable += delta;
      break;
    case OPERATION_RECOVERING:
      recovering += delta;
      break;
    case OPERATION_UNSUPPORTED:
    case OPERATION_UNKNOWN:
      LOG(ERROR) << "Unexpected operation state: " << operationState;
      break;
  }
}


void Metrics::incrementOperationState(
    Offer::Operation::Type type,
    const OperationState& state)
{
  operation_states.update(state, 1);
  if (operation_type_states.count(type)) {
    operation_type_states.at(type).update(state, 1);
  }
}


void Metrics::decrementOperationState(
    Offer::Operation::Type type,
    const OperationState& state)
{
  operation_states.update(state, -1);
  if (operation_type_states.count(type)) {
    operation_type_states.at(type).update(state, -1);
  }
}


void Metrics::transitionOperationState(
    Offer::Operation::Type type,
    const OperationState& oldState,
    const OperationState& newState)
{
  if (oldState == newState) {
    return;  // Nothing to do.
  }

  decrementOperationState(type, oldState);
  incrementOperationState(type, newState);
}


FrameworkMetrics::FrameworkMetrics(
    const FrameworkInfo& _frameworkInfo,
    bool _publishPerFrameworkMetrics)
  : metricPrefix(getFrameworkMetricPrefix(_frameworkInfo)),
    publishPerFrameworkMetrics(_publishPerFrameworkMetrics),
    subscribed(metricPrefix + "subscribed"),
    calls(metricPrefix + "calls"),
    events(metricPrefix + "events"),
    offers_sent(metricPrefix + "offers/sent"),
    offers_accepted(metricPrefix + "offers/accepted"),
    offers_declined(metricPrefix + "offers/declined"),
    offers_rescinded(metricPrefix + "offers/rescinded"),
    operations(metricPrefix + "operations")
{
  addMetric(subscribed);
  addMetric(offers_sent);
  addMetric(offers_accepted);
  addMetric(offers_declined);
  addMetric(offers_rescinded);

  // Add metrics for scheduler calls.
  addMetric(calls);
  for (int index = 0;
       index < scheduler::Call::Type_descriptor()->value_count();
       index++) {
    const google::protobuf::EnumValueDescriptor* descriptor =
      scheduler::Call::Type_descriptor()->value(index);

    const scheduler::Call::Type type =
      static_cast<scheduler::Call::Type>(descriptor->number());

    if (type == scheduler::Call::UNKNOWN) {
      continue;
    }

    Counter counter = Counter(
        metricPrefix + "calls/" + strings::lower(descriptor->name()));

    call_types.put(type, counter);
    addMetric(counter);
  }

  // Add metrics for scheduler events.
  addMetric(events);
  for (int index = 0;
       index < scheduler::Event::Type_descriptor()->value_count();
       index++) {
    const google::protobuf::EnumValueDescriptor* descriptor =
      scheduler::Event::Type_descriptor()->value(index);

    const scheduler::Event::Type type =
      static_cast<scheduler::Event::Type>(descriptor->number());

    if (type == scheduler::Event::UNKNOWN) {
      continue;
    }

    Counter counter = Counter(
        metricPrefix + "events/" + strings::lower(descriptor->name()));

    event_types.put(type, counter);
    addMetric(counter);
  }

  // Add metrics for both active and terminal task states.
  for (int index = 0; index < TaskState_descriptor()->value_count(); index++) {
    const google::protobuf::EnumValueDescriptor* descriptor =
      TaskState_descriptor()->value(index);

    const TaskState state = static_cast<TaskState>(descriptor->number());

    if (protobuf::isTerminalState(state)) {
      Counter counter = Counter(
          metricPrefix + "tasks/terminal/" +
          strings::lower(descriptor->name()));

      terminal_task_states.put(state, counter);
      addMetric(counter);
    } else {
      PushGauge gauge = PushGauge(
          metricPrefix + "tasks/active/" +
          strings::lower(TaskState_Name(state)));

      active_task_states.put(state, gauge);
      addMetric(gauge);
    }
  }

  // Add metrics for offer operations.
  addMetric(operations);
  for (int index = 0;
       index < Offer::Operation::Type_descriptor()->value_count();
       index++) {
    const google::protobuf::EnumValueDescriptor* descriptor =
      Offer::Operation::Type_descriptor()->value(index);

    const Offer::Operation::Type type =
      static_cast<Offer::Operation::Type>(descriptor->number());

    if (type == Offer::Operation::UNKNOWN) {
      continue;
    }

    Counter counter = Counter(
        metricPrefix + "operations/" + strings::lower(descriptor->name()));

    operation_types.put(type, counter);
    addMetric(counter);
  }
}


FrameworkMetrics::~FrameworkMetrics()
{
  removeMetric(subscribed);

  removeMetric(calls);
  foreachvalue (const Counter& counter, call_types) {
    removeMetric(counter);
  }

  process::metrics::remove(events);
  foreachvalue (const Counter& counter, event_types) {
    removeMetric(counter);
  }

  removeMetric(offers_sent);
  removeMetric(offers_accepted);
  removeMetric(offers_declined);
  removeMetric(offers_rescinded);

  foreachvalue (const Counter& counter, terminal_task_states) {
    removeMetric(counter);
  }

  foreachvalue (const PushGauge& gauge, active_task_states) {
    removeMetric(gauge);
  }

  process::metrics::remove(operations);
  foreachvalue (const Counter& counter, operation_types) {
    removeMetric(counter);
  }
}


void FrameworkMetrics::incrementCall(const scheduler::Call::Type& callType)
{
  CHECK(call_types.contains(callType));

  ++call_types.at(callType);
  ++calls;
}


void FrameworkMetrics::incrementTaskState(const TaskState& state)
{
  if (protobuf::isTerminalState(state)) {
    CHECK(terminal_task_states.contains(state));
    ++terminal_task_states.at(state);
  } else {
    CHECK(active_task_states.contains(state));
    ++active_task_states.at(state);
  }
}


void FrameworkMetrics::decrementActiveTaskState(const TaskState& state)
{
  CHECK(active_task_states.contains(state));

  active_task_states.at(state) -= 1;
}


void FrameworkMetrics::incrementOperation(const Offer::Operation& operation)
{
  CHECK(operation_types.contains(operation.type()));

  ++operation_types.at(operation.type());
  ++operations;
}


string getFrameworkMetricPrefix(const FrameworkInfo& frameworkInfo)
{
  // Percent-encode the framework name to avoid characters like '/' and ' '.
  return "master/frameworks/" + process::http::encode(frameworkInfo.name()) +
    "/" + stringify(frameworkInfo.id()) + "/";
}


template <typename T>
void FrameworkMetrics::addMetric(const T& metric)  {
  if (publishPerFrameworkMetrics) {
    process::metrics::add(metric);
  }
}


template <typename T>
void FrameworkMetrics::removeMetric(const T& metric)  {
  if (publishPerFrameworkMetrics) {
    process::metrics::remove(metric);
  }
}


void FrameworkMetrics::incrementEvent(const scheduler::Event& event)
{
  ++CHECK_NOTNONE(event_types.get(event.type()));
  ++events;
}


void FrameworkMetrics::incrementEvent(
    const FrameworkErrorMessage& message)
{
  ++CHECK_NOTNONE(event_types.get(scheduler::Event::ERROR));
  ++events;
}


void FrameworkMetrics::incrementEvent(
    const ExitedExecutorMessage& message)
{
  ++CHECK_NOTNONE(event_types.get(scheduler::Event::FAILURE));
  ++events;
}


void FrameworkMetrics::incrementEvent(
    const LostSlaveMessage& message)
{
  ++CHECK_NOTNONE(event_types.get(scheduler::Event::FAILURE));
  ++events;
}


void FrameworkMetrics::incrementEvent(
    const InverseOffersMessage& message)
{
  ++CHECK_NOTNONE(event_types.get(scheduler::Event::INVERSE_OFFERS));
  ++events;
}


void FrameworkMetrics::incrementEvent(
    const ExecutorToFrameworkMessage& message)
{
  ++CHECK_NOTNONE(event_types.get(scheduler::Event::MESSAGE));
  ++events;
}


void FrameworkMetrics::incrementEvent(
    const ResourceOffersMessage& message)
{
  ++CHECK_NOTNONE(event_types.get(scheduler::Event::OFFERS));
  ++events;
}


void FrameworkMetrics::incrementEvent(
    const RescindResourceOfferMessage& message)
{
  ++CHECK_NOTNONE(event_types.get(scheduler::Event::RESCIND));
  ++events;
}


void FrameworkMetrics::incrementEvent(
    const RescindInverseOfferMessage& message)
{
  ++CHECK_NOTNONE(event_types.get(scheduler::Event::RESCIND_INVERSE_OFFER));
  ++events;
}


void FrameworkMetrics::incrementEvent(
    const FrameworkRegisteredMessage& message)
{
  ++CHECK_NOTNONE(event_types.get(scheduler::Event::SUBSCRIBED));
  ++events;
}


void FrameworkMetrics::incrementEvent(
    const FrameworkReregisteredMessage& message)
{
  ++CHECK_NOTNONE(event_types.get(scheduler::Event::SUBSCRIBED));
  ++events;
}


void FrameworkMetrics::incrementEvent(
    const StatusUpdateMessage& message)
{
  ++CHECK_NOTNONE(event_types.get(scheduler::Event::UPDATE));
  ++events;
}


void FrameworkMetrics::incrementEvent(
    const UpdateOperationStatusMessage& message)
{
  ++CHECK_NOTNONE(event_types.get(scheduler::Event::UPDATE_OPERATION_STATUS));
  ++events;
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
