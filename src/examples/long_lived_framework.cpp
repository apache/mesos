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

#include <glog/logging.h>

#include <iostream>
#include <queue>
#include <string>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>
#include <mesos/v1/scheduler.hpp>

#include <mesos/authorizer/acls.hpp>

#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/time.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>

#include <stout/os/realpath.hpp>

#include "common/parse.hpp"

#include "examples/flags.hpp"

#include "logging/logging.hpp"

using std::queue;
using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

using mesos::v1::AgentID;
using mesos::v1::CommandInfo;
using mesos::v1::Credential;
using mesos::v1::ExecutorID;
using mesos::v1::ExecutorInfo;
using mesos::v1::Filters;
using mesos::v1::FrameworkID;
using mesos::v1::FrameworkInfo;
using mesos::v1::Offer;
using mesos::v1::Resources;
using mesos::v1::TaskInfo;
using mesos::v1::TaskState;
using mesos::v1::TaskStatus;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;
using mesos::v1::scheduler::Mesos;

using process::AUTHENTICATION;
using process::Clock;
using process::defer;
using process::DESCRIPTION;
using process::HELP;
using process::Owned;
using process::TLDR;

using process::http::OK;

using process::metrics::PullGauge;
using process::metrics::Counter;

// NOTE: Per-task resources are nominal because all of the resources for the
// container are provisioned when the executor is created. The executor can
// run multiple tasks at once, but uses a constant amount of resources
// regardless of the number of tasks.
const double CPUS_PER_TASK = 0.001;
const int32_t MEM_PER_TASK = 1;

const double CPUS_PER_EXECUTOR = 0.1;
const int32_t MEM_PER_EXECUTOR = 32;

constexpr char EXECUTOR_BINARY[] = "long-lived-executor";
constexpr char EXECUTOR_NAME[] = "Long Lived Executor (C++)";
constexpr char FRAMEWORK_NAME[] = "Long Lived Framework (C++)";
constexpr char FRAMEWORK_METRICS_PREFIX[] = "long_lived_framework";


// This scheduler picks one agent and repeatedly launches sleep tasks on it,
// using a single multi-task executor. If the agent or executor fails, the
// scheduler will pick another agent and continue launching sleep tasks.
class LongLivedScheduler : public process::Process<LongLivedScheduler>
{
public:
  LongLivedScheduler(
      const string& _master,
      const FrameworkInfo& _framework,
      const ExecutorInfo& _executor,
      const Option<Credential>& _credential)
    : state(DISCONNECTED),
      master(_master),
      framework(_framework),
      role(_framework.roles(0)),
      executor(_executor),
      taskResources([this]() {
        Resources resources = Resources::parse(
            "cpus:" + stringify(CPUS_PER_TASK) +
            ";mem:" + stringify(MEM_PER_TASK)).get();
        resources.allocate(this->role);
        return resources;
      }()),
      tasksLaunched(0),
      credential(_credential),
      metrics(*this)
  {
    start_time = Clock::now();
  }

  ~LongLivedScheduler() override {}

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

  void received(queue<Event> events)
  {
    while (!events.empty()) {
      Event event = events.front();
      events.pop();

      LOG(INFO) << "Received " << event.type() << " event";

      switch (event.type()) {
        case Event::SUBSCRIBED: {
          framework.mutable_id()->CopyFrom(event.subscribed().framework_id());

          LOG(INFO) << "Subscribed with ID '" << framework.id() << "'";

          state = SUBSCRIBED;
          break;
        }

        case Event::OFFERS: {
          offers(google::protobuf::convert(event.offers().offers()));
          break;
        }

        case Event::UPDATE: {
          update(event.update().status());
          break;
        }

        // TODO(greggomann): Implement handling of operation status updates.
        case Event::UPDATE_OPERATION_STATUS:
          break;

        case Event::FAILURE: {
          const Event::Failure& failure = event.failure();

          if (failure.has_agent_id() && failure.has_executor_id()) {
            executorFailed(
                failure.executor_id(),
                failure.agent_id(),
                failure.has_status() ? Option<int>(failure.status()) : None());
          } else {
            CHECK(failure.has_agent_id());

            agentFailed(failure.agent_id());
          }
          break;
        }

        case Event::ERROR: {
          EXIT(EXIT_FAILURE) << "Error: " << event.error().message();
          break;
        }

        case Event::HEARTBEAT:
        case Event::INVERSE_OFFERS:
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

  void offers(const vector<Offer>& offers)
  {
    CHECK_EQ(SUBSCRIBED, state);

    const Resources executorResources = [this]() {
      Resources resources(executor.resources());
      resources.allocate(role);
      return resources;
    }();

    metrics.offers_received += offers.size();

    foreach (const Offer& offer, offers) {
      if (agentId.isNone()) {
        // No active executor running in the cluster.
        // Launch a new task with executor.

        if (Resources(offer.resources())
              .toUnreserved()
              .contains(taskResources + executorResources)) {
          LOG(INFO) << "Starting executor and task " << tasksLaunched << " on "
                    << offer.hostname();

          launch(offer);

          agentId = offer.agent_id();
        } else {
          decline(offer);
        }
      } else if (agentId == offer.agent_id()) {
        // Offer from the same agent that has an active executor.
        // Launch more tasks on that executor.

        if (Resources(offer.resources())
              .toUnreserved()
              .contains(taskResources)) {
          LOG(INFO) << "Starting task " << tasksLaunched << " on "
                    << offer.hostname();

          launch(offer);
        } else {
          decline(offer);
        }
      } else {
        // We have an active executor but this offer comes from a
        // different agent; decline the offer.
        decline(offer);
      }
    }
  }

  void update(const TaskStatus& status)
  {
    CHECK_EQ(SUBSCRIBED, state);

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

    if (status.state() == TaskState::TASK_KILLED ||
        status.state() == TaskState::TASK_LOST ||
        status.state() == TaskState::TASK_FAILED ||
        status.state() == TaskState::TASK_ERROR) {
      // Launch on an invalid offer should not be
      // counted as abnormal termination.
      if (status.reason() != TaskStatus::REASON_INVALID_OFFERS) {
        ++metrics.abnormal_terminations;
      }
    }
  }

  void agentFailed(const AgentID& _agentId)
  {
    CHECK_EQ(SUBSCRIBED, state);

    LOG(INFO) << "Agent lost: " << _agentId;

    if (agentId == _agentId) {
      agentId = None();
    }
  }

  void executorFailed(
      const ExecutorID& executorId,
      const AgentID& _agentId,
      const Option<int>& status)
  {
    CHECK_EQ(SUBSCRIBED, state);

    LOG(INFO)
      << "Executor '" << executorId << "' lost on agent '" << _agentId
      << (status.isSome() ? "' with status: " + stringify(status.get()) : "");

    agentId = None();
  }

private:
  // Helper to decline an offer.
  void decline(const Offer& offer)
  {
    Filters filters;
    filters.set_refuse_seconds(600);

    Call call;
    call.set_type(Call::DECLINE);

    CHECK(framework.has_id());
    call.mutable_framework_id()->CopyFrom(framework.id());

    Call::Decline* decline = call.mutable_decline();
    decline->add_offer_ids()->CopyFrom(offer.id());
    decline->mutable_filters()->CopyFrom(filters);

    mesos->send(call);
  }

  // Helper to launch a task using an offer.
  void launch(const Offer& offer)
  {
    int taskId = tasksLaunched++;
    ++metrics.tasks_launched;

    TaskInfo task;
    task.set_name("Task " + stringify(taskId));
    task.mutable_task_id()->set_value(stringify(taskId));
    task.mutable_agent_id()->MergeFrom(offer.agent_id());
    task.mutable_resources()->CopyFrom(taskResources);
    task.mutable_executor()->CopyFrom(executor);

    Call call;
    call.set_type(Call::ACCEPT);

    CHECK(framework.has_id());
    call.mutable_framework_id()->CopyFrom(framework.id());

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    Offer::Operation* operation = accept->add_operations();
    operation->set_type(Offer::Operation::LAUNCH);

    operation->mutable_launch()->add_task_infos()->CopyFrom(task);

    mesos->send(call);
  }

  enum State
  {
    DISCONNECTED,
    CONNECTED,
    SUBSCRIBED
  } state;

  const string master;
  FrameworkInfo framework;
  const string role;
  const ExecutorInfo executor;
  const Resources taskResources;
  string uri;
  int tasksLaunched;
  const Option<Credential> credential;

  // The agent that is running the long-lived-executor.
  // Unless that agent/executor dies, this framework will not launch
  // an executor on any other agent.
  Option<AgentID> agentId;

  Owned<Mesos> mesos;

  process::Time start_time;
  double _uptime_secs()
  {
    return (Clock::now() - start_time).secs();
  }

  double _subscribed()
  {
    return state == SUBSCRIBED ? 1 : 0;
  }

  struct Metrics
  {
    Metrics(const LongLivedScheduler& scheduler)
      : uptime_secs(
            string(FRAMEWORK_METRICS_PREFIX) + "/uptime_secs",
            defer(scheduler, &LongLivedScheduler::_uptime_secs)),
        subscribed(
            string(FRAMEWORK_METRICS_PREFIX) + "/subscribed",
            defer(scheduler, &LongLivedScheduler::_subscribed)),
        offers_received(
            string(FRAMEWORK_METRICS_PREFIX) + "/offers_received"),
        tasks_launched(
            string(FRAMEWORK_METRICS_PREFIX) + "/tasks_launched"),
        abnormal_terminations(
            string(FRAMEWORK_METRICS_PREFIX) + "/abnormal_terminations")
    {
      process::metrics::add(uptime_secs);
      process::metrics::add(subscribed);
      process::metrics::add(offers_received);
      process::metrics::add(tasks_launched);
      process::metrics::add(abnormal_terminations);
    }

    ~Metrics()
    {
      process::metrics::remove(uptime_secs);
      process::metrics::remove(subscribed);
      process::metrics::remove(offers_received);
      process::metrics::remove(tasks_launched);
      process::metrics::remove(abnormal_terminations);
    }

    process::metrics::PullGauge uptime_secs;
    process::metrics::PullGauge subscribed;

    process::metrics::Counter offers_received;
    process::metrics::Counter tasks_launched;

    // The only expected terminal state is TASK_FINISHED.
    // Other terminal states are considered incorrect.
    process::metrics::Counter abnormal_terminations;
  } metrics;
};


class Flags : public virtual mesos::internal::examples::Flags
{
public:
  Flags()
  {
    add(&Flags::build_dir,
        "build_dir",
        "The build directory of Mesos. If set, the framework will assume\n"
        "that the executor, framework, and agent(s) all live on the same\n"
        "machine.");

    add(&Flags::executor_uri,
        "executor_uri",
        "URI the fetcher should use to get the executor's binary.\n"
        "NOTE: This flag is deprecated in favor of `--executor_uris`");

    add(&Flags::executor_uris,
        "executor_uris",
        "The value could be a JSON-formatted string of `URI`s that\n"
        "should be fetched before running the executor, or a file\n"
        "path containing the JSON-formatted `URI`s. Path must be of\n"
        "the form `file:///path/to/file` or `/path/to/file`.\n"
        "This flag replaces `--executor_uri`.\n"
        "See the `CommandInfo::URI` message in `mesos.proto` for the\n"
        "expected format.\n"
        "Example:\n"
        "[\n"
        "  {\n"
        "    \"value\":\"mesos.apache.org/balloon_executor\",\n"
        "    \"executable\":\"true\"\n"
        "  },\n"
        "  {\n"
        "    \"value\":\"mesos.apache.org/bundle_for_executor.tar.gz\",\n"
        "    \"cache\":\"true\"\n"
        "  }\n"
        "]");

    add(&Flags::executor_command,
        "executor_command",
        "The command that should be used to start the executor.\n"
        "This will override the value set by `--build_dir`.");
  }

  // Flags for specifying the executor binary and other URIs.
  //
  // TODO(armand): Remove the `--executor_uri` flag after the
  // deprecation cycle, started in 1.4.0.
  Option<string> build_dir;
  Option<string> executor_uri;
  Option<JSON::Array> executor_uris;
  Option<string> executor_command;
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

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  const Resources resources = Resources::parse(
      "cpus:" + stringify(CPUS_PER_EXECUTOR) +
      ";mem:" + stringify(MEM_PER_EXECUTOR)).get();

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.mutable_resources()->CopyFrom(resources);
  executor.set_name(EXECUTOR_NAME);

  // Determine the command to run the executor based on three possibilities:
  //   1) `--executor_command` was set, which overrides the below cases.
  //   2) We are in the Mesos build directory, so the targeted executable
  //      is actually a libtool wrapper script.
  //   3) We have not detected the Mesos build directory, so assume the
  //      executor is in the same directory as the framework.
  string command;

  // Find this executable's directory to locate executor.
  if (flags.executor_command.isSome()) {
    command = flags.executor_command.get();
  } else if (flags.build_dir.isSome()) {
    command = path::join(flags.build_dir.get(), "src", EXECUTOR_BINARY);
  } else {
    command =
      path::join(os::realpath(Path(argv[0]).dirname()).get(), EXECUTOR_BINARY);
  }

  executor.mutable_command()->set_value(command);

  if (flags.executor_uris.isSome() && flags.executor_uri.isSome()) {
    EXIT(EXIT_FAILURE)
      << "Flag '--executor_uris' shall not be used with '--executor_uri'";
  }

  // Copy `--executor_uri` into the command.
  if (flags.executor_uri.isSome()) {
    LOG(WARNING)
      << "Flag '--executor_uri' is deprecated, use '--executor_uris' instead";

    CommandInfo::URI* uri = executor.mutable_command()->add_uris();
    uri->set_value(flags.executor_uri.get());
    uri->set_executable(true);
  }

  // Copy `--executor_uris` into the command.
  if (flags.executor_uris.isSome()) {
    Try<RepeatedPtrField<CommandInfo::URI>> parse =
      ::protobuf::parse<RepeatedPtrField<CommandInfo::URI>>(
          flags.executor_uris.get());

    if (parse.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to convert '--executor_uris' to protobuf: " << parse.error();
    }

    executor.mutable_command()->mutable_uris()->CopyFrom(parse.get());
  }

  FrameworkInfo framework;
  framework.set_user(os::user().get());
  framework.set_principal(flags.principal);
  framework.set_name(FRAMEWORK_NAME);
  framework.set_checkpoint(flags.checkpoint);
  framework.add_roles(flags.role);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  Option<Credential> credential = None();

  if (flags.authenticate) {
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

  Owned<LongLivedScheduler> scheduler(new LongLivedScheduler(
      flags.master,
      framework,
      executor,
      credential));

  process::spawn(scheduler.get());
  process::wait(scheduler.get());

  return EXIT_SUCCESS;
}
