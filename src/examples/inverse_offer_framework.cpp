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

#include <iostream>
#include <queue>
#include <string>
#include <vector>

#include <mesos/authorizer/acls.hpp>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>
#include <mesos/v1/scheduler.hpp>

#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/time.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>
#include <process/metrics/pull_gauge.hpp>

#include <stout/check.hpp>
#include <stout/exit.hpp>
#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>

#include "examples/flags.hpp"

#include "logging/logging.hpp"

using namespace mesos::v1;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;

using process::Clock;
using process::defer;

using process::http::OK;

using process::metrics::Counter;
using process::metrics::PullGauge;

const float CPUS_PER_TASK = 0.2;
const int32_t MEM_PER_TASK = 32;

constexpr char FRAMEWORK_NAME[] = "Inverse Offer Framework (C++)";
constexpr char FRAMEWORK_METRICS_PREFIX[] = "inverse_offer_framework";


// Holds a sleep task and when the task's machine is scheduled for maintenance.
struct SleeperInfo
{
  TaskID taskId;
  TimeInfo unavailability;
};


// This scheduler launches and maintains a configurable number of
// infinite-sleep tasks, placing at most one task on a single agent.
// When the operator schedules maintenance on the cluster, the scheduler
// will respond by migrating sleep tasks ahead of the planned maintenance.
class InverseOfferScheduler : public process::Process<InverseOfferScheduler>
{
public:
  InverseOfferScheduler(
      const FrameworkInfo& _framework,
      const std::string& _master,
      const uint32_t _num_tasks,
      const Option<Credential>& _credential)
    : framework(_framework),
      master(_master),
      num_tasks(_num_tasks),
      credential(_credential),
      tasks_launched(0),
      state(DISCONNECTED),
      metrics(*this)
  {
    start_time = Clock::now();
  }

  ~InverseOfferScheduler() override {}

protected:
  void initialize() override
  {
    // We initialize the library here to ensure that callbacks are only invoked
    // after the process has spawned.
    mesos.reset(new scheduler::Mesos(
        master,
        mesos::ContentType::PROTOBUF,
        process::defer(self(), &Self::connected),
        process::defer(self(), &Self::disconnected),
        process::defer(self(), &Self::received, lambda::_1),
        credential));
  }

  void connected()
  {
    state = CONNECTED;

    doReliableRegistration();
  }

  void disconnected()
  {
    LOG(INFO) << "Disconnected!";

    state = DISCONNECTED;
  }

  void doReliableRegistration()
  {
    if (state == SUBSCRIBED || state == DISCONNECTED) {
      return;
    }

    Call call;
    call.set_type(Call::SUBSCRIBE);

    if (framework.has_id()) {
      call.mutable_framework_id()->CopyFrom(framework.id());
    }

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(framework);

    mesos->send(call);

    process::delay(Seconds(1), self(), &Self::doReliableRegistration);
  }

  void received(std::queue<Event> events)
  {
    while (!events.empty()) {
      Event event = events.front();
      events.pop();

      LOG(INFO) << "Received " << event.type() << " event";

      switch (event.type()) {
        case Event::SUBSCRIBED: {
          framework.mutable_id()->CopyFrom(event.subscribed().framework_id());

          LOG(INFO) << "Subscribed with ID '" << framework.id();
          state = SUBSCRIBED;
          break;
        }

        case Event::OFFERS: {
          metrics.offers_received += event.offers().offers().size();

          resourceOffers(google::protobuf::convert(event.offers().offers()));
          break;
        }

        case Event::INVERSE_OFFERS: {
          metrics.inverse_offers_received +=
            event.inverse_offers().inverse_offers().size();

          inverseOffers(google::protobuf::convert(
              event.inverse_offers().inverse_offers()));
          break;
        }

        case Event::UPDATE: {
          statusUpdate(event.update().status());
          break;
        }

        // TODO(greggomann): Implement handling of operation status updates.
        case Event::UPDATE_OPERATION_STATUS:
          break;

        case Event::FAILURE: {
          const Event::Failure& failure = event.failure();

          if (failure.has_agent_id() && failure.has_executor_id()) {
            LOG(INFO)
              << "Executor '" << failure.executor_id()
              << "' lost on agent '" << failure.agent_id()
              << (failure.has_status() ?
                  "' with status: " + stringify(failure.status()) : "");
          } else {
            CHECK(failure.has_agent_id());

            LOG(INFO) << "Agent lost: " << failure.agent_id();
          }
          break;
        }

        case Event::ERROR: {
          EXIT(EXIT_FAILURE) << "Error: " << event.error().message();
          break;
        }

        case Event::HEARTBEAT:
        case Event::RESCIND:
        case Event::RESCIND_INVERSE_OFFER:
        case Event::MESSAGE: {
          break;
        }

        case Event::UNKNOWN: {
          LOG(WARNING) << "Received an UNKNOWN event and ignored";
          break;
        }
      }
    }
  }

private:
  void resourceOffers(const std::vector<Offer>& offers)
  {
    CHECK(framework.has_id());

    // Of existing sleep tasks, identify the one running on an agent
    // with the least expected uptime (i.e. next to be maintained).
    // We'll see if we can migrate this sleep task.
    Option<AgentID> riskiestAgent;
    foreachpair (const AgentID& agentId, const SleeperInfo& sleeper, sleepers) {
      if (riskiestAgent.isSome()) {
        if (sleeper.unavailability.nanoseconds() <
            sleepers[riskiestAgent.get()].unavailability.nanoseconds()) {
          riskiestAgent = agentId;
        }
      } else if (sleeper.unavailability.nanoseconds() > 0) {
        riskiestAgent = agentId;
      }
    }

    foreach (const Offer& offer, offers) {
      const Resources taskResources = [this]() {
        Resources resources = Resources::parse(
            "cpus:" + stringify(CPUS_PER_TASK) +
            ";mem:" + stringify(MEM_PER_TASK)).get();
        resources.allocate(framework.role());
        return resources;
      }();

      // Are there already `num_task` sleep tasks running?
      // Having `num_task` sleeps running takes priority over dealing
      // with maintenance.
      bool needMoreSleep = sleepers.size() < num_tasks;

      // Is the agent in the offer less risky than our riskiest agent?
      // i.e. The offered agent's planned downtime is farther away.
      bool offeredAgentIsLessRisky = riskiestAgent.isSome() &&
          (!offer.has_unavailability() ||
            offer.unavailability().start().nanoseconds() >
            sleepers[riskiestAgent.get()].unavailability.nanoseconds());

      // Are we already running a task on this agent?
      // This scheduler will only launch one task per agent.
      bool offeredAgentIsOccupied = sleepers.contains(offer.agent_id());

      // We only need to accept an offer if we do not have enough sleep
      // tasks active, or the offer provides a better agent.
      bool needToLaunchTask = !offeredAgentIsOccupied &&
        (needMoreSleep || offeredAgentIsLessRisky);

      Resources resources(offer.resources());

      // Check if this offer is big enough and if we need to launch anything.
      if (!resources.toUnreserved().contains(taskResources) ||
          !needToLaunchTask) {
        Call call;
        call.mutable_framework_id()->CopyFrom(framework.id());
        call.set_type(Call::DECLINE);

        Call::Decline* decline = call.mutable_decline();
        decline->add_offer_ids()->CopyFrom(offer.id());
        decline->mutable_filters()->set_refuse_seconds(600);

        mesos->send(call);
        continue;
      }

      // Keeping `num_tasks` running has higher priority than migrating tasks.
      // We only migrate tasks if there are enough running tasks.
      if (!needMoreSleep && offeredAgentIsLessRisky) {
        LOG(INFO) << "Migrating task " << sleepers[riskiestAgent.get()].taskId
                  << " from " << riskiestAgent.get();

        Call call;
        call.mutable_framework_id()->CopyFrom(framework.id());
        call.set_type(Call::KILL);

        Call::Kill* kill = call.mutable_kill();
        kill->mutable_task_id()->CopyFrom(sleepers[riskiestAgent.get()].taskId);
        kill->mutable_agent_id()->CopyFrom(riskiestAgent.get());

        mesos->send(call);

        // Keep track of this sleeper in another map.
        migrating[riskiestAgent.get()] = sleepers[riskiestAgent.get()];
        sleepers.erase(riskiestAgent.get());

        // For simplicity, we only migrate one task per round of offers.
        // Setting the `riskiestAgent` means we will no longer consider
        // migrating tasks in this loop.
        riskiestAgent = None();

        // Since we killed a task, we need to start another one.
        needMoreSleep = true;
      }

      if (needMoreSleep) {
        LOG(INFO) << "Starting task " << tasks_launched
                  << " on " << offer.agent_id();

        TaskInfo task;
        task.mutable_task_id()->set_value(stringify(tasks_launched));
        task.set_name("Sleeper Agent " + stringify(tasks_launched++));
        task.mutable_agent_id()->MergeFrom(offer.agent_id());
        task.mutable_resources()->CopyFrom(taskResources);
        task.mutable_command()->set_value(
            "while [ true ]; do echo ZZZzzz...; sleep 5; done");

        Call call;
        call.mutable_framework_id()->CopyFrom(framework.id());
        call.set_type(Call::ACCEPT);

        Call::Accept* accept = call.mutable_accept();
        accept->add_offer_ids()->CopyFrom(offer.id());

        Offer::Operation* operation = accept->add_operations();
        operation->set_type(Offer::Operation::LAUNCH);
        operation->mutable_launch()->add_task_infos()->CopyFrom(task);

        mesos->send(call);

        // Save the new sleep task.
        SleeperInfo sleeper;
        sleeper.taskId = task.task_id();
        if (offer.has_unavailability()) {
          sleeper.unavailability = offer.unavailability().start();
        }
        sleepers[offer.agent_id()] = sleeper;
      }
    }
  }


  void inverseOffers(const std::vector<InverseOffer>& offers)
  {
    foreach (const InverseOffer& offer, offers) {
      if (!sleepers.contains(offer.agent_id())) {
        LOG(INFO) << "Inverse offer received for " << offer.agent_id()
                  << " which does not hold an active sleep task.";
        continue;
      }

      // Take note of any agents that are scheduled for maintenance.
      sleepers[offer.agent_id()].unavailability =
        offer.unavailability().start();

      // TODO(josephw): Demonstrate some semantics for declining inverse
      // offers. This framework currently always accepts inverse offers.
      Call call;
      CHECK(framework.has_id());
      call.mutable_framework_id()->CopyFrom(framework.id());

      call.set_type(Call::ACCEPT_INVERSE_OFFERS);
      Call::AcceptInverseOffers* accept = call.mutable_accept_inverse_offers();
      accept->add_inverse_offer_ids()->CopyFrom(offer.id());

      mesos->send(call);
    }
  }


  void statusUpdate(const TaskStatus& status)
  {
    LOG(INFO)
      << "Task " << status.task_id().value()
      << " is in state " << TaskState_Name(status.state())
      << (status.has_message() ? " with message: " + status.message() : "");

    if (status.has_uuid()) {
      Call call;
      call.set_type(Call::ACKNOWLEDGE);

      CHECK(framework.has_id());
      call.mutable_framework_id()->CopyFrom(framework.id());

      Call::Acknowledge* acknowledge = call.mutable_acknowledge();
      acknowledge->mutable_agent_id()->CopyFrom(status.agent_id());
      acknowledge->mutable_task_id()->CopyFrom(status.task_id());
      acknowledge->set_uuid(status.uuid());

      mesos->send(call);
    }

    // This is the only expected terminal state.
    if (migrating.contains(status.agent_id()) &&
        status.state() == TASK_KILLED) {
      ++metrics.sleepers_killed;

      migrating.erase(status.agent_id());
      return;
    }

    // These are un-expected terminal states.
    if (status.state() == TASK_FINISHED ||
        status.state() == TASK_LOST ||
        status.state() == TASK_FAILED ||
        status.state() == TASK_ERROR ||
        status.state() == TASK_KILLED) {
      ++metrics.sleepers_lost_abnormally;

      sleepers.erase(status.agent_id());
      migrating.erase(status.agent_id());
    }
  }

  void finalize() override
  {
    if (state == SUBSCRIBED) {
      Call call;
      CHECK(framework.has_id());
      call.mutable_framework_id()->CopyFrom(framework.id());
      call.set_type(Call::TEARDOWN);

      mesos->send(call);
    }
  }

  FrameworkInfo framework;
  const std::string master;
  const uint32_t num_tasks;
  const Option<Credential> credential;

  // Agents which currently hold a sleep task.
  hashmap<AgentID, SleeperInfo> sleepers;
  hashmap<AgentID, SleeperInfo> migrating;

  int tasks_launched;

  process::Owned<scheduler::Mesos> mesos;

  enum State
  {
    DISCONNECTED,
    CONNECTED,
    SUBSCRIBED
  } state;

  process::Time start_time;
  double _uptime_secs()
  {
    return (Clock::now() - start_time).secs();
  }

  double _subscribed()
  {
    return state == SUBSCRIBED ? 1 : 0;
  }

  double _current_sleepers()
  {
    return sleepers.size() + migrating.size();
  }

  struct Metrics
  {
    Metrics(const InverseOfferScheduler& scheduler)
      : uptime_secs(
          std::string(FRAMEWORK_METRICS_PREFIX) + "/uptime_secs",
          defer(scheduler, &InverseOfferScheduler::_uptime_secs)),
      subscribed(
          std::string(FRAMEWORK_METRICS_PREFIX) + "/subscribed",
          defer(scheduler, &InverseOfferScheduler::_subscribed)),
      offers_received(
          std::string(FRAMEWORK_METRICS_PREFIX) + "/offers_received"),
      inverse_offers_received(
          std::string(FRAMEWORK_METRICS_PREFIX) + "/inverse_offers_received"),
      sleepers_killed(
          std::string(FRAMEWORK_METRICS_PREFIX) + "/sleepers_killed"),
      sleepers_lost_abnormally(
          std::string(FRAMEWORK_METRICS_PREFIX) + "/sleepers_lost_abnormally"),
      current_sleepers(
          std::string(FRAMEWORK_METRICS_PREFIX) + "/current_sleepers",
          defer(scheduler, &InverseOfferScheduler::_current_sleepers))
    {
      process::metrics::add(uptime_secs);
      process::metrics::add(subscribed);
      process::metrics::add(offers_received);
      process::metrics::add(inverse_offers_received);
      process::metrics::add(sleepers_killed);
      process::metrics::add(sleepers_lost_abnormally);
      process::metrics::add(current_sleepers);
    }

    ~Metrics()
    {
      process::metrics::remove(uptime_secs);
      process::metrics::remove(subscribed);
      process::metrics::remove(offers_received);
      process::metrics::remove(inverse_offers_received);
      process::metrics::remove(sleepers_killed);
      process::metrics::remove(sleepers_lost_abnormally);
      process::metrics::remove(current_sleepers);
    }

    process::metrics::PullGauge uptime_secs;
    process::metrics::PullGauge subscribed;

    process::metrics::Counter offers_received;
    process::metrics::Counter inverse_offers_received;

    // The only expected terminal state is TASK_KILLED.
    // Other terminal states are considered incorrect.
    process::metrics::Counter sleepers_killed;
    process::metrics::Counter sleepers_lost_abnormally;

    process::metrics::PullGauge current_sleepers;
  } metrics;
};


class Flags : public virtual mesos::internal::examples::Flags
{
public:
  Flags()
  {
    add(&Flags::num_tasks,
        "num_tasks",
        "Number of sleep tasks to run at once. Each task is started on\n"
        "a separate machine. The scheduler will attempt to migrate tasks\n"
        "to other machines ahead of planned maintenance.",
        1,
        [](int value) -> Option<Error> {
          if (value <= 0) {
            return Error("Expected --num_tasks greater than zero");
          }

          return None();
        });
  }

  int num_tasks;
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

  mesos::internal::logging::initialize(argv[0], true, flags); // Catch signals.

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  // Nothing special to say about this framework.
  FrameworkInfo framework;
  framework.set_user(os::user().get());
  framework.set_name(FRAMEWORK_NAME);
  framework.set_role(flags.role);
  framework.set_checkpoint(flags.checkpoint);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

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

  if (flags.master == "local") {
    // Configure master.
    os::setenv("MESOS_ROLES", flags.role);

    os::setenv(
        "MESOS_AUTHENTICATE_HTTP_FRAMEWORKS",
        stringify(flags.authenticate));

    os::setenv("MESOS_HTTP_FRAMEWORK_AUTHENTICATORS", "basic");

    mesos::ACLs acls;
    mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->add_values(flags.role);
    os::setenv("MESOS_ACLS", stringify(JSON::protobuf(acls)));
  }

  process::Owned<InverseOfferScheduler> scheduler(new InverseOfferScheduler(
      framework,
      flags.master,
      flags.num_tasks,
      credential));

  process::spawn(scheduler.get());
  process::wait(scheduler.get());

  return EXIT_SUCCESS;
}
