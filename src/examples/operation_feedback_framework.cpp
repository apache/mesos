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
#include <vector>

#include <mesos/type_utils.hpp>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>
#include <mesos/v1/scheduler.hpp>

#include <mesos/authorizer/acls.hpp>

#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/time.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/check.hpp>
#include <stout/exit.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/uuid.hpp>

#include "common/protobuf_utils.hpp"

#include "examples/flags.hpp"

#include "logging/logging.hpp"

using process::Clock;
using process::Owned;

using std::string;
using std::vector;

using mesos::ACL;
using mesos::ACLs;

using mesos::v1::AgentID;
using mesos::v1::Credential;
using mesos::v1::FrameworkID;
using mesos::v1::FrameworkInfo;
using mesos::v1::Label;
using mesos::v1::Offer;
using mesos::v1::OperationID;
using mesos::v1::OperationState;
using mesos::v1::OperationStatus;
using mesos::v1::Resource;
using mesos::v1::Resources;
using mesos::v1::TaskID;
using mesos::v1::TaskInfo;
using mesos::v1::TaskStatus;

using mesos::v1::OPERATION_FINISHED;
using mesos::v1::OPERATION_GONE_BY_OPERATOR;
using mesos::v1::OPERATION_FAILED;
using mesos::v1::OPERATION_ERROR;
using mesos::v1::OPERATION_DROPPED;
using mesos::v1::OPERATION_RECOVERING;
using mesos::v1::OPERATION_PENDING;
using mesos::v1::OPERATION_UNREACHABLE;
using mesos::v1::OPERATION_UNKNOWN;
using mesos::v1::OPERATION_UNSUPPORTED;

using mesos::v1::TASK_DROPPED;
using mesos::v1::TASK_LOST;
using mesos::v1::TASK_ERROR;
using mesos::v1::TASK_FAILED;
using mesos::v1::TASK_FINISHED;
using mesos::v1::TASK_GONE;
using mesos::v1::TASK_GONE_BY_OPERATOR;
using mesos::v1::TASK_KILLING;
using mesos::v1::TASK_KILLED;
using mesos::v1::TASK_RUNNING;
using mesos::v1::TASK_STAGING;
using mesos::v1::TASK_STARTING;
using mesos::v1::TASK_UNREACHABLE;
using mesos::v1::TASK_UNKNOWN;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;
using mesos::v1::scheduler::Mesos;


namespace {

constexpr char FRAMEWORK_NAME[] = "Operation Feedback Framework (C++)";
constexpr char FRAMEWORK_METRICS_PREFIX[] = "operation_feedback_framework";
constexpr char RESERVATIONS_LABEL[] = "operation_feedback_framework_label";
constexpr Duration RECONCILIATION_INTERVAL = Seconds(30);
constexpr Duration RESUBSCRIPTION_INTERVAL = Seconds(2);
constexpr Seconds REFUSE_TIME = Seconds::max();

} // namespace {


// This framework will run a set of given tasks on a Mesos cluster.
// For each task, the framework will attempt to `RESERVE` task resources,
// then `LAUNCH` the task and finally `UNRESERVE` task resources.
//
// This is very similar to what the `DynamicReservationFramework` does, but
// this one does so using the v1 scheduler API and offer operation feedback
// to showcase both of these features.
//
// In particular, if everything works smoothly every task passes sequentially
// through the following lifecycle stages:
//
//   1) AWAITING_RESERVE_OFFER
//      The framework is waiting for a suitable offer containing
//      resources to reserve.
//
//   2) AWAITING_RESERVE_ACK
//      The framework attempted to reserve resources and is awaiting
//      confirmation via offer operation feedback.
//
//   3) AWAITING_LAUNCH_OFFER
//      The resources for this task have been successfully reserved, and it
//      is awaiting a suitable offer to launch the task on the reservation.
//
//   4) AWAITING_TASK_FINISHED
//      The task has been launched, and the framework is waiting for it to
//      finish.
//
//   5) AWAITING_UNRESERVE_OFFER
//      The task has finished successfully, and the framework is waiting for
//      another offer containing this task's reservation so it can clean up.
//
//   6) AWAITING_UNRESERVE_ACK
//      The framework attempted to unreserve the resources for this task.
//
//
// Note that some failure conditions are not handled by this example framework:
//  - If an agent containing the reservation for one of the tasks is permanently
//    removed before the task finished, this is not detected and no attempt is
//    made to move the reservation to another agent. Instead, the task will be
//    left waiting for an offer from that agent forever.
//
//  - If the framework is killed or shut down before all reservations have been
//    unreserved, these left-over reservations require manual cleanup.

class OperationFeedbackScheduler
  : public process::Process<OperationFeedbackScheduler>
{
  struct SchedulerTask;

public:
  OperationFeedbackScheduler(
      const FrameworkInfo& _framework,
      const string& _master,
      const string& _role,
      const Option<Credential>& _credential,
      bool _cleanupUnknownReservations)
    : framework(_framework),
      master(_master),
      role(_role),
      credential(_credential),
      cleanupUnknownReservations(_cleanupUnknownReservations),
      reservationsLabelValue(id::UUID::random().toString()),
      metrics(*this)
  {
    startTime = Clock::now();

    LOG(INFO) << "Tagging reservations with label {'" << RESERVATIONS_LABEL
              << "': '" << reservationsLabelValue << "'}";
  }

  ~OperationFeedbackScheduler() override {}

  void addTask(const string& command, const Resources& resources)
  {
    static int taskIdCounter = 0;
    ++taskIdCounter;

    Resource::ReservationInfo reservationInfo;
    reservationInfo.set_type(Resource::ReservationInfo::DYNAMIC);
    reservationInfo.set_role(role);
    reservationInfo.set_principal(framework.principal());

    Label* label = reservationInfo.mutable_labels()->add_labels();
    label->set_key(RESERVATIONS_LABEL);
    label->set_value(reservationsLabelValue);

    SchedulerTask task;
    task.stage = SchedulerTask::AWAITING_RESERVE_OFFER;
    task.taskResources = resources;
    task.taskResources.allocate(role);
    // The task will run on reserved resources.
    Resources taskResourcesReserved =
      task.taskResources.pushReservation(reservationInfo);

    task.taskInfo.mutable_resources()->CopyFrom(taskResourcesReserved);
    task.taskInfo.mutable_command()->set_shell(true);
    task.taskInfo.mutable_command()->set_value(command);
    task.taskInfo.mutable_task_id()->set_value(
        "task-" + stringify(taskIdCounter));
    task.taskInfo.set_name(
        "Operation Feedback Task " + stringify(taskIdCounter));

    tasks.push_back(task);
    return;
  }

protected:
  void initialize() override
  {
    // We initialize the library here to ensure that callbacks are only invoked
    // after the process has spawned.
    mesos.reset(
        new Mesos(
            master,
            mesos::ContentType::PROTOBUF,
            process::defer(self(), &Self::connected),
            process::defer(self(), &Self::disconnected),
            process::defer(self(), &Self::received, lambda::_1),
            credential));
  }

  void finalize() override
  {
    if (framework.has_id()) {
      Call call;
      call.mutable_framework_id()->CopyFrom(framework.id());
      call.set_type(Call::TEARDOWN);
      mesos->send(call);
    }
  }

  void connected()
  {
    LOG(INFO) << "Connected";
    reconcileOperations();
    subscribe();
  }

  void subscribe()
  {
    LOG(INFO) << "Sending `SUBSCRIBE` call to Mesos master";
    // The master can respond with an error to the `SUBSCRIBE` call, but the
    // `Mesos` class will just silently swallow that error, leaving us hanging
    // forever with no clue what's going on.
    // In particular, running this with the `--master=local` flag frequently
    // results in `503 Service Unavailable` responses.
    // Therefore, we have to retry in a loop until we're actually registered.
    if (!framework.has_id()) {
      mesos->send(SUBSCRIBE(framework));
      process::delay(RESUBSCRIPTION_INTERVAL, self(), &Self::subscribe);
    }
  }

  void disconnected()
  {
    EXIT(EXIT_FAILURE) << "Disconnected";
  }

  void received(std::queue<Event> events)
  {
    while (!events.empty()) {
      Event event = events.front();
      events.pop();

      VLOG(1) << "Received " << event.type() << " event";

      switch (event.type()) {
        case Event::SUBSCRIBED: {
          isSubscribed = true;
          framework.mutable_id()->CopyFrom(event.subscribed().framework_id());
          LOG(INFO) << "Subscribed with ID '" << framework.id();
          break;
        }

        case Event::OFFERS: {
          resourceOffers(google::protobuf::convert(event.offers().offers()));
          break;
        }

        case Event::UPDATE: {
          taskStatusUpdate(event.update().status());
          break;
        }

        case Event::UPDATE_OPERATION_STATUS: {
          operationStatusUpdate(event.update_operation_status().status());
          break;
        }

        case Event::ERROR: {
          EXIT(EXIT_FAILURE) << "Error: " << event.error().message();
          break;
        }

        default:
          break;
      }
    }
  }

  void resourceOffers(const vector<Offer>& offers)
  {
    foreach (const Offer& offer, offers) {
      VLOG(1) << "Offer " << offer.id()
              << " from agent " << offer.agent_id()
              << " contained resources " << offer.resources();

      Call call;
      call.set_type(Call::ACCEPT);
      call.mutable_framework_id()->CopyFrom(framework.id());
      Call::Accept* accept = call.mutable_accept();
      accept->add_offer_ids()->CopyFrom(offer.id());
      accept->mutable_filters()->set_refuse_seconds(REFUSE_TIME.secs());

      Resources remaining(offer.resources());
      int reservations = 0, launches = 0, unreservations = 0;
      bool havePendingTasks = false;  // Whether any task awaits an offer.

      // Cleanup reservations from previous runs.
      //
      // NOTE: We don't set an operation ID for these unreservations, so we
      // don't expect to receive any operation status updates for them.
      if (cleanupUnknownReservations) {
        Resources reserved = remaining.reserved(role);
        foreach (const Resource& resource, reserved) {
          foreach (Label label, resource.reservations(0).labels().labels()) {
            if (label.key() != RESERVATIONS_LABEL) {
              continue;
            }

            if (label.value() != reservationsLabelValue) {
              LOG(INFO) << "Removing reservation made by another instance "
                        << "of the framework: " << resource;

              Offer::Operation* operation = accept->add_operations();
              operation->set_type(Offer::Operation::UNRESERVE);
              operation->mutable_unreserve()->add_resources()->CopyFrom(
                  resource);

              remaining -= resource;
              ++unreservations;
            }
          }
        }
      }

      // For each pending task that does not yet have a reservation,
      // check whether the offer contains enough unreserved resources
      // and if so, attempt to reserve them.
      foreachTaskInState(SchedulerTask::AWAITING_RESERVE_OFFER,
          [&] (SchedulerTask& task) {
        havePendingTasks = true;
        if (remaining.contains(task.taskResources)) {
          LOG(INFO) << "Reserving resources for task "
                    << task.taskInfo.task_id();

          Offer::Operation* operation = accept->add_operations();
          operation->set_type(Offer::Operation::RESERVE);
          std::string id = "reserve-" + id::UUID::random().toString();
          operation->mutable_id()->set_value(id);
          operation->mutable_reserve()->mutable_resources()->CopyFrom(
              task.taskInfo.resources());

          remaining -= task.taskResources;
          task.taskInfo.mutable_agent_id()->CopyFrom(offer.agent_id());
          task.reserveOperationId = operation->id();
          task.stage = SchedulerTask::AWAITING_RESERVE_ACK;
          ++reservations;
        }
      });

      // For each pending task that had its resources reserved successfully,
      // attempt to launch the task.
      foreachTaskInState(SchedulerTask::AWAITING_LAUNCH_OFFER,
          [&] (SchedulerTask& task) {
        CHECK(task.taskInfo.has_agent_id());
        havePendingTasks = true;
        if (offer.agent_id() == task.taskInfo.agent_id() &&
            remaining.contains(task.taskInfo.resources())) {
          LOG(INFO) << "Launching task " << task.taskInfo.task_id();

          Offer::Operation* operation = accept->add_operations();
          operation->set_type(Offer::Operation::LAUNCH);
          operation->mutable_launch()->add_task_infos()->CopyFrom(
              task.taskInfo);

          task.stage = SchedulerTask::AWAITING_TASK_FINISHED;
          ++launches;
        }
      });

      // For each task that finished running, attempt to unreserve its
      // resources.
      foreachTaskInState(SchedulerTask::AWAITING_UNRESERVE_OFFER,
          [&] (SchedulerTask& task) {
        CHECK(task.taskInfo.has_agent_id());
        havePendingTasks = true;
        if (offer.agent_id() == task.taskInfo.agent_id()) {
          LOG(INFO) << "Unreserving resources for task "
                    << task.taskInfo.task_id();

          Offer::Operation* operation = accept->add_operations();
          operation->set_type(Offer::Operation::UNRESERVE);
          std::string id = "unreserve-" + id::UUID::random().toString();
          operation->mutable_id()->set_value(id);
          operation->mutable_unreserve()->mutable_resources()->CopyFrom(
              task.taskInfo.resources());

          task.unreserveOperationId = operation->id();
          task.stage = SchedulerTask::AWAITING_UNRESERVE_ACK;
          ++unreservations;
        }
      });

      if (havePendingTasks) {
        LOG(INFO) << "Accepting offer with "
          << reservations << " `RESERVE` operations, "
          << launches << " `LAUNCH` operations and "
          << unreservations << " `UNRESERVE` operations while having "
          << tasks.size() << " non-completed tasks";
      }

      // Each `ACCEPT` call must only contain offers with the same agent
      // id, so we have to send one call per offer back to the master.
      // We also want to send the `ACCEPT` call if we don't launch any
      // operations on this offer, in order to decline the offer.
      mesos->send(call);
    }
  }

  void taskStatusUpdate(const TaskStatus& status)
  {
    VLOG(1) << "Received status update " << status;

    const TaskID& taskId = status.task_id();

    auto taskIterator = std::find_if(tasks.begin(), tasks.end(),
      [&] (const SchedulerTask& tx) {
        return tx.taskInfo.task_id() == taskId;
      });

    if (taskIterator == tasks.end()) {
      LOG(WARNING) << "Status update for unknown task " << taskId;
      return;
    }

    if (status.has_uuid()) {
      mesos->send(ACKNOWLEDGE(status, framework.id()));
    }

    if (taskIterator->stage != SchedulerTask::AWAITING_TASK_FINISHED) {
      // We could get spurious updates due to the reconciliation process
      // or because of retries; ignore them.
      return;
    }

    switch (status.state()) {
      case TASK_STAGING:
      case TASK_KILLING:
      case TASK_STARTING:
      case TASK_RUNNING:
      case TASK_UNREACHABLE: {
        // Nothing to do yet, wait for further updates.
        VLOG(1) << "Received " << status.state() << " for task " << taskId;
        break;
      }

      case TASK_DROPPED:
      case TASK_ERROR:
      case TASK_KILLED:
      case TASK_GONE:
      case TASK_GONE_BY_OPERATOR:
      case TASK_UNKNOWN:
      case TASK_LOST:
      case TASK_FAILED: {
        LOG(INFO) << "Task " << taskId << " failed, attempting to relaunch"
                  << " with the next offer";

        taskIterator->stage = SchedulerTask::AWAITING_LAUNCH_OFFER;
        break;
      }

      case TASK_FINISHED: {
        LOG(INFO) << "Task " << taskId << " finished, attempting to unreserve"
                  << " its resources with the next offer";

        taskIterator->stage = SchedulerTask::AWAITING_UNRESERVE_OFFER;
        break;
      }
    }
  }

  void operationStatusUpdate(const OperationStatus& status)
  {
    VLOG(1) << "Received operation status update " << status;

    if (status.has_uuid()) {
      mesos->send(ACKNOWLEDGE_OPERATION(status, framework.id()));
    }

    const OperationID& operationId = status.operation_id();

    auto taskIterator = std::find_if(tasks.begin(), tasks.end(),
        [&] (const SchedulerTask& tx) {
          return tx.reserveOperationId == operationId;
        });

    if (taskIterator != tasks.end()) {
      handleReserveOperationStatusUpdate(taskIterator, status);
      return;
    }

    taskIterator = std::find_if(tasks.begin(), tasks.end(),
        [&] (const SchedulerTask& tx) {
          return tx.unreserveOperationId == operationId;
        });

    if (taskIterator != tasks.end()) {
      handleUnreserveOperationStatusUpdate(taskIterator, status);
      return;
    }

    LOG(WARNING) << "Status update for unknown operation " << operationId;
  }

  void handleReserveOperationStatusUpdate(
      typename std::list<SchedulerTask>::iterator task,
      const OperationStatus& status)
  {
    if (task->stage != SchedulerTask::AWAITING_RESERVE_ACK) {
      // We could get spurious updates due to the reconciliation process
      // or because of retries; ignore them.
      return;
    }

    switch (status.state()) {
      case OPERATION_PENDING:
      case OPERATION_RECOVERING:
      case OPERATION_UNKNOWN:
      case OPERATION_UNREACHABLE: {
        // Nothing to do but wait.
        break;
      }

      case OPERATION_UNSUPPORTED: {
        // Someone in the future invented an additional operation state
        // and accidentally sent it to us.
        break;
      }

      case OPERATION_FAILED:
      case OPERATION_ERROR:
      case OPERATION_DROPPED:
      case OPERATION_GONE_BY_OPERATOR: {
        LOG(INFO)
          << "Received update " << status << " attempting to reserve "
          << " resources for task " << task->taskInfo.task_id()
          << "; retrying";

        task->stage = SchedulerTask::AWAITING_RESERVE_OFFER;
        break;
      }

      case OPERATION_FINISHED: {
        LOG(INFO) << "Successfully reserved resources for task "
                  << task->taskInfo.task_id() << "; awaiting launch offer";

        task->stage = SchedulerTask::AWAITING_LAUNCH_OFFER;

        LOG(INFO) << "Reviving offers for role '"  << role << "'";

        Call call;
        call.set_type(Call::REVIVE);
        call.mutable_framework_id()->CopyFrom(framework.id());
        call.mutable_revive()->add_roles(role);

        mesos->send(call);

        break;
      }
    }
  }

  void handleUnreserveOperationStatusUpdate(
      typename std::list<SchedulerTask>::iterator task,
      const OperationStatus& status)
  {
    if (task->stage != SchedulerTask::AWAITING_UNRESERVE_ACK) {
      // We could get spurious updates due to the reconciliation
      // or because of retries; ignore them.
      return;
    }

    switch (status.state()) {
      case OPERATION_PENDING:
      case OPERATION_RECOVERING:
      case OPERATION_UNKNOWN:
      case OPERATION_UNREACHABLE: {
        // Nothing to do but wait.
        break;
      }

      case OPERATION_UNSUPPORTED: {
        // Someone in the future invented an additional operation state
        // and accidentally sent it to us.
        break;
      }

      case OPERATION_FAILED:
      case OPERATION_ERROR:
      case OPERATION_DROPPED: {
        LOG(INFO)
          << "Received update " << status << " attempting to unreserve "
          << " resources for task " << task->taskInfo.task_id()
          << "; retrying";

        task->stage = SchedulerTask::AWAITING_UNRESERVE_OFFER;
        break;
      }

      case OPERATION_FINISHED:
      case OPERATION_GONE_BY_OPERATOR: {
        // We also count `GONE_BY_OPERATOR` as a success, because in that
        // case there's nothing left to unreserve.
        LOG(INFO) << "Task " << task->taskInfo.task_id() << " done; removing";
        tasks.erase(task);
        if (tasks.empty()) {
          LOG(INFO) << "All tasks completed, shutting down the framework...";
          process::terminate(self());
        }
        break;
      }
    }
  }

  void reconcileOperations() {
    // Keep reconciling as long as the framework is running.
    process::delay(RECONCILIATION_INTERVAL, self(), &Self::reconcileOperations);

    if (!framework.has_id()) {
      return;
    }

    Call call;
    call.set_type(Call::RECONCILE_OPERATIONS);
    call.mutable_framework_id()->CopyFrom(framework.id());
    Call::ReconcileOperations* reconcile = call.mutable_reconcile_operations();

    for (const SchedulerTask& task : tasks) {
      switch (task.stage) {
        case SchedulerTask::AWAITING_RESERVE_OFFER:
        case SchedulerTask::AWAITING_UNRESERVE_OFFER:
        case SchedulerTask::AWAITING_LAUNCH_OFFER:
        case SchedulerTask::AWAITING_TASK_FINISHED: {
          // Not currently waiting for any offer feedback.
          break;
        }

        case SchedulerTask::AWAITING_RESERVE_ACK: {
          Call::ReconcileOperations::Operation* operation =
            reconcile->add_operations();
          operation->mutable_agent_id()->MergeFrom(task.taskInfo.agent_id());
          operation->mutable_operation_id()->MergeFrom(
              task.reserveOperationId.get());
        }

        case SchedulerTask::AWAITING_UNRESERVE_ACK: {
          Call::ReconcileOperations::Operation* operation =
            reconcile->add_operations();
          operation->mutable_agent_id()->MergeFrom(task.taskInfo.agent_id());
          operation->mutable_operation_id()->MergeFrom(
              task.unreserveOperationId.get());
        }
      }
    }

    mesos->send(call);
  }

private:
  // Static helper functions to deal with protobuf generation:

  static Call SUBSCRIBE(const FrameworkInfo& framework)
  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    if (framework.has_id()) {
      call.mutable_framework_id()->CopyFrom(framework.id());
    }

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(framework);

    return call;
  }

  static Call ACKNOWLEDGE(
      const TaskStatus& status,
      const FrameworkID& frameworkId)
  {
    CHECK(status.has_uuid());
    Call call;
    call.set_type(Call::ACKNOWLEDGE);
    call.mutable_framework_id()->CopyFrom(frameworkId);

    Call::Acknowledge* acknowledge = call.mutable_acknowledge();
    acknowledge->mutable_agent_id()->CopyFrom(status.agent_id());
    acknowledge->mutable_task_id()->CopyFrom(status.task_id());
    acknowledge->set_uuid(std::string(status.uuid()));

    return call;
  }

  static Call ACKNOWLEDGE_OPERATION(
      const OperationStatus& status,
      const FrameworkID& frameworkId)
  {
    CHECK(status.has_uuid());
    Call call;
    call.set_type(Call::ACKNOWLEDGE_OPERATION_STATUS);
    call.mutable_framework_id()->CopyFrom(frameworkId);

    Call::AcknowledgeOperationStatus* acknowledge =
      call.mutable_acknowledge_operation_status();
    acknowledge->mutable_agent_id()->CopyFrom(status.agent_id());
    acknowledge->mutable_operation_id()->CopyFrom(status.operation_id());
    acknowledge->set_uuid(status.uuid().value());

    return call;
  }

  Owned<Mesos> mesos;
  FrameworkInfo framework;
  string master;
  string role;
  Option<Credential> credential;

  // See `cleanup_unknown_reservations` flag description below.
  bool cleanupUnknownReservations;

  // The value for the label that will be used to mark reservations made by the
  // current instance of the framework.
  string reservationsLabelValue;

  // Represents a task lifecycle from the scheduler's perspective, i.e.
  // a reserve operation followed by a mesos task followed by an unreserve
  // operation.
  struct SchedulerTask {
    enum Stage {
      AWAITING_RESERVE_OFFER,
      AWAITING_RESERVE_ACK,
      AWAITING_LAUNCH_OFFER,
      AWAITING_TASK_FINISHED,
      AWAITING_UNRESERVE_OFFER,
      AWAITING_UNRESERVE_ACK,
    };

    Stage stage;

    TaskInfo taskInfo;
    // Since the resources inside `taskInfo` already contain a
    // `ReservationInfo`, we store an extra copy without that as a
    // convenience for offer matching.
    Resources taskResources;
    Option<OperationID> reserveOperationId;
    Option<OperationID> unreserveOperationId;
  };

  std::list<SchedulerTask> tasks;

  // This function needs to have `SchedulerTask::Stage` be already defined.
  template<typename UnaryOperation>
  void foreachTaskInState(
      SchedulerTask::Stage stage,
      UnaryOperation f)
  {
    for (SchedulerTask& task : tasks) {
      if (task.stage == stage) {
        f(task);
      }
    }
  }

  process::Time startTime;
  double _uptime_secs()
  {
    return (Clock::now() - startTime).secs();
  }

  bool isSubscribed;
  double _subscribed()
  {
    return isSubscribed ? 1 : 0;
  }

  struct Metrics
  {
    Metrics(const OperationFeedbackScheduler& _scheduler)
      : uptime_secs(
            string(FRAMEWORK_METRICS_PREFIX) + "/uptime_secs",
            defer(_scheduler, &OperationFeedbackScheduler::_uptime_secs)),
        subscribed(
            string(FRAMEWORK_METRICS_PREFIX) + "/subscribed",
            defer(_scheduler, &OperationFeedbackScheduler::_subscribed))
    {
      process::metrics::add(uptime_secs);
      process::metrics::add(subscribed);
    }

    ~Metrics()
    {
      process::metrics::remove(uptime_secs);
      process::metrics::remove(subscribed);
    }

    process::metrics::PullGauge uptime_secs;
    process::metrics::PullGauge subscribed;
  } metrics;
};


class Flags : public virtual mesos::internal::examples::Flags
{
public:
  Flags()
  {
    add(&Flags::user,
        "user",
        "The username under which to run tasks.");

    add(&Flags::cleanup_unknown_reservations,
        "cleanup_unknown_reservations",
        "Cleanup reservations not made by this instance of the framework.\n"
        "This should only be enabled if no other framework is making\n"
        "reservations under the same role.",
        true);

    add(&Flags::command,
        "command",
        "The command to run for each task.",
        "sleep 60");

    add(&Flags::resources,
        "resources",
        "The resources to reserve for each task.",
        "cpus:1;mem:32");

    add(&Flags::num_tasks,
        "num_tasks",
        "The number of task.",
        60);
  }

  string command;
  string resources;
  Option<string> user;
  int num_tasks;
  bool cleanup_unknown_reservations;
};


int main(int argc, char** argv)
{
  Flags flags;
  Try<flags::Warnings> load = flags.load("MESOS_EXAMPLE_", argc, argv);

  if (flags.help) {
    std::cout << flags.usage() << std::endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    std::cerr << flags.usage(load.error()) << std::endl;
    return EXIT_FAILURE;
  }

  mesos::internal::logging::initialize(argv[0], false);
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  if (flags.role == "*") {
    EXIT(EXIT_FAILURE) << flags.usage(
        "Role is incorrect; the default '*' role cannot be used");
  }

  Option<Credential> credential = None();

  if (flags.authenticate) {
    LOG(INFO) << "Enabling authentication for the framework";

    Credential credential_;
    credential_.set_principal(flags.principal);
    if (flags.secret.isSome()) {
      credential_.set_secret(flags.secret.get());
    }
    credential = credential_;
  }

  Try<Resources> parsedResources = Resources::parse(flags.resources);
  if (parsedResources.isError()) {
    EXIT(EXIT_FAILURE)
      << "Failed to parse resources: "  << parsedResources.error();
  }

  FrameworkInfo framework;
  framework.set_user(flags.user.isSome() ? flags.user.get() : os::user().get());
  framework.set_principal(flags.principal);
  framework.set_name(FRAMEWORK_NAME);
  framework.set_checkpoint(flags.checkpoint);
  framework.add_roles(flags.role);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  LOG(INFO) << "Starting OperationFeedbackScheduler for " << flags.num_tasks
            << " tasks with command " << flags.command;

  OperationFeedbackScheduler scheduler(
      framework,
      flags.master,
      flags.role,
      credential,
      flags.cleanup_unknown_reservations);

  for (int i=0; i < flags.num_tasks; ++i) {
    scheduler.addTask(flags.command, parsedResources.get());
  }

  if (flags.master == "local") {
    // Configure master. The constructor of `Mesos()` will load all
    // environment variables prefixed by `MESOS_`.
    os::setenv("MESOS_AUTHENTICATE_FRAMEWORKS", stringify(flags.authenticate));

    ACLs acls;
    ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_roles()->add_values(flags.role);
    os::setenv("MESOS_ACLS", stringify(JSON::protobuf(acls)));
  }

  process::spawn(&scheduler);
  process::wait(&scheduler);

  return EXIT_SUCCESS;
}
