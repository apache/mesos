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
#include <string>
#include <vector>

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/authorizer/acls.hpp>

#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/time.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/bytes.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>

#include <stout/os/realpath.hpp>

#include "common/parse.hpp"

#include "examples/flags.hpp"

#include "logging/logging.hpp"

using namespace mesos;
using namespace mesos::internal;

using std::string;

using google::protobuf::RepeatedPtrField;

using process::Clock;
using process::defer;

using process::metrics::PullGauge;
using process::metrics::Counter;

const double CPUS_PER_TASK = 0.1;

const double CPUS_PER_EXECUTOR = 0.1;
const int32_t MEM_PER_EXECUTOR = 64;

constexpr char EXECUTOR_BINARY[] = "balloon-executor";
constexpr char FRAMEWORK_METRICS_PREFIX[] = "balloon_framework";


class Flags : public virtual mesos::internal::examples::Flags
{
public:
  Flags()
  {
    add(&Flags::name,
        "name",
        "Name to be used by the framework.",
        "Balloon Framework");

    add(&Flags::task_memory_usage_limit,
        "task_memory_usage_limit",
        None(),
        "Maximum size, in bytes, of the task's memory usage.\n"
        "The task will attempt to occupy memory up until this limit.",
        static_cast<const Bytes*>(nullptr),
        [](const Bytes& value) -> Option<Error> {
          if (value < Bytes(MEM_PER_EXECUTOR, Bytes::MEGABYTES)) {
            return Error(
                "Please use a --task_memory_usage_limit greater than " +
                stringify(MEM_PER_EXECUTOR) + " MB");
          }

          return None();
        });

    add(&Flags::task_memory,
        "task_memory",
        "How much memory the framework will require per task.\n"
        "If not specified, the task(s) will use all available memory in\n"
        "applicable offers.");

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

    add(&Flags::long_running,
        "long_running",
        "Whether this framework should launch tasks repeatedly\n"
        "or exit after finishing a single task.",
        false);
  }

  string name;
  Bytes task_memory_usage_limit;
  Bytes task_memory;

  // Flags for specifying the executor binary and other URIs.
  //
  // TODO(armand): Remove the `--executor_uri` flag after the
  // deprecation cycle, started in 1.4.0.
  Option<string> build_dir;
  Option<string> executor_uri;
  Option<JSON::Array> executor_uris;
  Option<string> executor_command;

  bool long_running;
};


// Actor holding the business logic and metrics for the `BalloonScheduler`.
// See `BalloonScheduler` below for the intended behavior.
class BalloonSchedulerProcess : public process::Process<BalloonSchedulerProcess>
{
public:
  BalloonSchedulerProcess(
      const FrameworkInfo& _frameworkInfo,
      const ExecutorInfo& _executor,
      const Flags& _flags)
    : frameworkInfo(_frameworkInfo),
      role(_frameworkInfo.roles(0)),
      executor(_executor),
      flags(_flags),
      taskActive(false),
      tasksLaunched(0),
      isRegistered(false),
      metrics(*this)
  {
    start_time = Clock::now();
  }

  void registered()
  {
    isRegistered = true;
  }

  void disconnected()
  {
    isRegistered = false;
  }

  void resourceOffers(
      SchedulerDriver* driver,
      const std::vector<Offer>& offers)
  {
    Resources taskResources = Resources::parse(
        "cpus:" + stringify(CPUS_PER_TASK) +
        ";mem:" + stringify(
            (double) flags.task_memory.bytes() / Bytes::MEGABYTES)).get();
    taskResources.allocate(role);

    Resources executorResources = Resources(executor.resources());
    executorResources.allocate(role);

    foreach (const Offer& offer, offers) {
      Resources resources(offer.resources());

      // If there is an active task, or if the offer is not
      // big enough, reject the offer.
      if (taskActive ||
          !resources.toUnreserved().contains(
              taskResources + executorResources)) {
        Filters filters;
        filters.set_refuse_seconds(600);

        driver->declineOffer(offer.id(), filters);
        continue;
      }

      int taskId = tasksLaunched++;

      LOG(INFO) << "Launching task " << taskId;

      TaskInfo task;
      task.set_name(flags.name + " Task");
      task.mutable_task_id()->set_value(stringify(taskId));
      task.mutable_slave_id()->MergeFrom(offer.slave_id());
      task.mutable_resources()->CopyFrom(taskResources);
      task.set_data(stringify(flags.task_memory_usage_limit));

      task.mutable_executor()->CopyFrom(executor);
      task.mutable_executor()->mutable_executor_id()->set_value(
          stringify(taskId));

      driver->launchTasks(offer.id(), {task});

      taskActive = true;
    }
  }

  void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {
    if (!flags.long_running) {
      if (status.state() == TASK_FAILED &&
          status.reason() == TaskStatus::REASON_CONTAINER_LIMITATION_MEMORY) {
        // NOTE: We expect TASK_FAILED when this scheduler is launched by the
        // balloon_framework_test.sh shell script. The abort here ensures the
        // script considers the test result as "PASS".
        driver->abort();
      } else if (status.state() == TASK_FAILED ||
          status.state() == TASK_FINISHED ||
          status.state() == TASK_KILLED ||
          status.state() == TASK_LOST ||
          status.state() == TASK_ERROR) {
        driver->stop();
      }
    }

    if (stringify(tasksLaunched - 1) != status.task_id().value()) {
      // We might receive messages from older tasks. Ignore them.
      LOG(INFO) << "Ignoring status update from older task "
                << status.task_id();
      return;
    }

    switch (status.state()) {
      case TASK_FINISHED:
        taskActive = false;
        ++metrics.tasks_finished;
        break;
      case TASK_FAILED:
        taskActive = false;
        if (status.reason() == TaskStatus::REASON_CONTAINER_LIMITATION_MEMORY) {
          ++metrics.tasks_oomed;
        }

        // NOTE: Fetching the executor (e.g. `--executor_uri`) may fail
        // occasionally if the URI is rate limited. This case is common
        // enough that it makes sense to track this failure metric separately.
        if (status.reason() == TaskStatus::REASON_CONTAINER_LAUNCH_FAILED) {
          ++metrics.launch_failures;
        }
        break;
      case TASK_KILLED:
      case TASK_LOST:
      case TASK_ERROR:
        taskActive = false;

        if (status.reason() != TaskStatus::REASON_INVALID_OFFERS) {
          ++metrics.abnormal_terminations;
        }
        break;
      case TASK_RUNNING:
        ++metrics.tasks_running;
        break;
      // We ignore uninteresting transient task status updates.
      case TASK_KILLING:
      case TASK_STAGING:
      case TASK_STARTING:
        break;
      // We ignore task status updates related to reconciliation.
      case TASK_DROPPED:
      case TASK_GONE:
      case TASK_GONE_BY_OPERATOR:
      case TASK_UNKNOWN:
      case TASK_UNREACHABLE:
        break;
    }
  }

private:
  const FrameworkInfo frameworkInfo;
  const string role;
  const ExecutorInfo executor;
  const Flags flags;
  bool taskActive;
  int tasksLaunched;

  process::Time start_time;
  double _uptime_secs()
  {
    return (Clock::now() - start_time).secs();
  }

  bool isRegistered;
  double _registered()
  {
    return isRegistered ? 1 : 0;
  }

  struct Metrics
  {
    Metrics(const BalloonSchedulerProcess& _scheduler)
      : uptime_secs(
            string(FRAMEWORK_METRICS_PREFIX) + "/uptime_secs",
            defer(_scheduler, &BalloonSchedulerProcess::_uptime_secs)),
        registered(
            string(FRAMEWORK_METRICS_PREFIX) + "/registered",
            defer(_scheduler, &BalloonSchedulerProcess::_registered)),
        tasks_finished(string(FRAMEWORK_METRICS_PREFIX) + "/tasks_finished"),
        tasks_oomed(string(FRAMEWORK_METRICS_PREFIX) + "/tasks_oomed"),
        tasks_running(string(FRAMEWORK_METRICS_PREFIX) + "/tasks_running"),
        launch_failures(string(FRAMEWORK_METRICS_PREFIX) + "/launch_failures"),
        abnormal_terminations(
            string(FRAMEWORK_METRICS_PREFIX) + "/abnormal_terminations")
    {
      process::metrics::add(uptime_secs);
      process::metrics::add(registered);
      process::metrics::add(tasks_finished);
      process::metrics::add(tasks_oomed);
      process::metrics::add(tasks_running);
      process::metrics::add(launch_failures);
      process::metrics::add(abnormal_terminations);
    }

    ~Metrics()
    {
      process::metrics::remove(uptime_secs);
      process::metrics::remove(registered);
      process::metrics::remove(tasks_finished);
      process::metrics::remove(tasks_oomed);
      process::metrics::remove(launch_failures);
      process::metrics::remove(abnormal_terminations);
    }

    process::metrics::PullGauge uptime_secs;
    process::metrics::PullGauge registered;

    process::metrics::Counter tasks_finished;
    process::metrics::Counter tasks_oomed;
    process::metrics::Counter tasks_running;
    process::metrics::Counter launch_failures;
    process::metrics::Counter abnormal_terminations;
  } metrics;
};


// This scheduler starts a single executor and task which gradually
// increases its memory footprint up to a limit.  Depending on the
// resource limits set for the container, the framework expects the
// executor to either finish successfully or be OOM-killed.
class BalloonScheduler : public Scheduler
{
public:
  BalloonScheduler(
      const FrameworkInfo& _frameworkInfo,
      const ExecutorInfo& _executor,
      const Flags& _flags)
    : process(_frameworkInfo, _executor, _flags)
  {
    process::spawn(process);
  }

  ~BalloonScheduler() override
  {
    process::terminate(process);
    process::wait(process);
  }

  void registered(
      SchedulerDriver*,
      const FrameworkID& frameworkId,
      const MasterInfo&) override
  {
    LOG(INFO) << "Registered with framework ID: " << frameworkId;

    process::dispatch(&process, &BalloonSchedulerProcess::registered);
  }

  void reregistered(SchedulerDriver*, const MasterInfo& masterInfo) override
  {
    LOG(INFO) << "Reregistered";

    process::dispatch(&process, &BalloonSchedulerProcess::registered);
  }

  void disconnected(SchedulerDriver* driver) override
  {
    LOG(INFO) << "Disconnected";

    process::dispatch(&process, &BalloonSchedulerProcess::disconnected);
  }

  void resourceOffers(
      SchedulerDriver* driver,
      const std::vector<Offer>& offers) override
  {
    LOG(INFO) << "Resource offers received";

    process::dispatch(
        &process,
        &BalloonSchedulerProcess::resourceOffers,
        driver,
        offers);
  }

  void offerRescinded(SchedulerDriver* driver, const OfferID& offerId) override
  {
    LOG(INFO) << "Offer rescinded";
  }

  void statusUpdate(SchedulerDriver* driver, const TaskStatus& status) override
  {
    LOG(INFO) << "Task " << status.task_id() << " in state "
              << TaskState_Name(status.state())
              << ", Source: " << status.source()
              << ", Reason: " << status.reason()
              << (status.has_message() ? ", Message: " + status.message() : "");

    process::dispatch(
        &process,
        &BalloonSchedulerProcess::statusUpdate,
        driver,
        status);
  }

  void frameworkMessage(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      const string& data) override
  {
    LOG(INFO) << "Framework message: " << data;
  }

  void slaveLost(SchedulerDriver* driver, const SlaveID& slaveId) override
  {
    LOG(INFO) << "Agent lost: " << slaveId;
  }

  void executorLost(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      int status) override
  {
    LOG(INFO) << "Executor '" << executorId << "' lost on agent: " << slaveId;
  }

  void error(SchedulerDriver* driver, const string& message) override
  {
    LOG(INFO) << "Error message: " << message;
  }

private:
  BalloonSchedulerProcess process;
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

  logging::initialize(argv[0], false);

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  const Resources resources = Resources::parse(
      "cpus:" + stringify(CPUS_PER_EXECUTOR) +
      ";mem:" + stringify(MEM_PER_EXECUTOR)).get();

  ExecutorInfo executor;
  executor.mutable_resources()->CopyFrom(resources);
  executor.set_name(flags.name + " Executor");

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

    mesos::CommandInfo::URI* uri = executor.mutable_command()->add_uris();
    uri->set_value(flags.executor_uri.get());
    uri->set_executable(true);
  }

  // Copy `--executor_uris` into the command.
  if (flags.executor_uris.isSome()) {
    Try<RepeatedPtrField<mesos::CommandInfo::URI>> parse =
      ::protobuf::parse<RepeatedPtrField<mesos::CommandInfo::URI>>(
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
  framework.set_name(flags.name);
  framework.set_checkpoint(flags.checkpoint);
  framework.add_roles(flags.role);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  BalloonScheduler scheduler(framework, executor, flags);

  if (flags.master == "local") {
    // Configure master.
    os::setenv("MESOS_ROLES", flags.role);
    os::setenv("MESOS_AUTHENTICATE_FRAMEWORKS", stringify(flags.authenticate));

    ACLs acls;
    ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_roles()->add_values(flags.role);
    os::setenv("MESOS_ACLS", stringify(JSON::protobuf(acls)));
  }

  MesosSchedulerDriver* driver;

  if (flags.authenticate) {
    LOG(INFO) << "Enabling authentication for the framework";

    Credential credential;
    credential.set_principal(flags.principal);
    if (flags.secret.isSome()) {
      credential.set_secret(flags.secret.get());
    }

    driver = new MesosSchedulerDriver(
        &scheduler,
        framework,
        flags.master,
        credential);
  } else {
    driver = new MesosSchedulerDriver(
        &scheduler,
        framework,
        flags.master);
  }

  int status = driver->run() == DRIVER_STOPPED ? 0 : 1;

  // Ensure that the driver process terminates.
  driver->stop();

  delete driver;
  return status;
}
