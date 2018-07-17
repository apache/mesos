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

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/authorizer/acls.hpp>

#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/time.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include "examples/flags.hpp"

#include "logging/logging.hpp"

using namespace mesos;

using std::string;

using process::Clock;
using process::defer;

using process::metrics::PullGauge;
using process::metrics::Counter;

const double CPUS_PER_TASK = 0.1;
const int MEMORY_PER_TASK = 16;
const Bytes DISK_PER_TASK = Megabytes(5);

constexpr char FRAMEWORK_METRICS_PREFIX[] = "disk_full_framework";


class Flags : public virtual mesos::internal::examples::Flags
{
public:
  Flags()
  {
    add(&Flags::name,
        "name",
        "Name to be used by the framework.",
        "Disk Full Framework");

    add(&Flags::run_once,
        "run_once",
        "Whether this framework should exit after running a single task.\n"
        "By default framework will keep running tasks forever.\n",
        false);

    add(&Flags::pre_sleep_duration,
        "pre_sleep_duration",
        "Duration of sleep before the task starts to consume the disk. The\n"
        "purpose of this is to allow an operator to control the frequency at\n"
        "which this framework consumes the available disk and terminates.\n"
        "The task will sleep for the specified duration before ramping up\n"
        "disk usage.\n",
        Seconds(600));

    add(&Flags::post_sleep_duration,
        "post_sleep_duration",
        "Duration of sleep after the task consumed the disk. The purpose\n"
        "of this is to allow an operator to control how long it takes for\n"
        "the task to terminate in case the disk quota enforcement does not\n"
        "take effect. The task will terminate after sleeping for specified\n"
        "duration.\n",
        Seconds(600));

    add(&Flags::disk_use_limit,
        "disk_use_limit",
        "The amount of disk (rounded to the nearest KB) the task should\n"
        "consume. The task requests 5MB of disk, so if the limit is set\n"
        "to exceed 5MB the expectation is that disk quota will be enforced\n"
        "and the task will terminated.\n");
  }

  string name;
  bool run_once;
  Duration pre_sleep_duration;
  Duration post_sleep_duration;
  Bytes disk_use_limit;
};

// Actor holding the business logic and metrics for the `DiskFullScheduler`.
// See `DiskFullScheduler` below for intended behavior.
class DiskFullSchedulerProcess
  : public process::Process<DiskFullSchedulerProcess>
{
public:
  DiskFullSchedulerProcess (
      const Flags& _flags,
      const FrameworkInfo& _frameworkInfo)
    : flags(_flags),
      frameworkInfo(_frameworkInfo),
      role(_frameworkInfo.roles(0)),
      tasksLaunched(0),
      taskActive(false),
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
        ";mem:" + stringify(MEMORY_PER_TASK) +
        ";disk:" + stringify(
            (double) DISK_PER_TASK.bytes() / Bytes::MEGABYTES)).get();
    taskResources.allocate(role);

    foreach (const Offer& offer, offers) {
      LOG(INFO) << "Received offer " << offer.id() << " from agent "
                << offer.slave_id() << " (" << offer.hostname() << ") "
                << "with " << offer.resources();

      Resources resources(offer.resources());

      // If we've already launched the task, or if the offer is not
      // big enough, reject the offer.
      if (taskActive || !resources.toUnreserved().contains(taskResources)) {
        Filters filters;
        filters.set_refuse_seconds(600);

        LOG(INFO) << "Declining offer " << offer.id() << ": "
                  << (taskActive ? "a task is already running"
                                 : "offer does not fit");

        driver->declineOffer(offer.id(), filters);
        continue;
      }

      int taskId = tasksLaunched++;

      // The task sleeps for the amount of seconds specified by the
      // pre_sleep_duration flag, ramps up the disk usage up to the limit
      // specified by `--use_disk_limit` and then sleeps for
      // post_sleep_duration more seconds.
      static const string command =
          "sleep " + stringify(flags.pre_sleep_duration.secs()) +
          " && dd if=/dev/zero of=file bs=1K count=" +
          stringify(flags.disk_use_limit.bytes() / Bytes::KILOBYTES) +
          " && sleep " + stringify(flags.post_sleep_duration.secs());

      TaskInfo task;
      task.set_name(flags.name + " Task");
      task.mutable_task_id()->set_value(stringify(taskId));
      task.mutable_slave_id()->MergeFrom(offer.slave_id());
      task.mutable_resources()->CopyFrom(taskResources);
      task.mutable_command()->set_shell(true);
      task.mutable_command()->set_value(command);

      LOG(INFO) << "Starting task " << taskId;

      driver->launchTasks(offer.id(), {task});

      taskActive = true;
    }
  }

  void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {
    if (stringify(tasksLaunched - 1) != status.task_id().value()) {
      // We might receive messages from older tasks. Ignore them.
      LOG(INFO) << "Ignoring status update from older task "
                << status.task_id();
      return;
    }

    switch (status.state()) {
    case TASK_FINISHED:
      if (flags.run_once) {
          driver->stop();
          break;
      }

      taskActive = false;
      ++metrics.tasks_finished;
      break;
    case TASK_FAILED:
      if (flags.run_once) {
          driver->abort();
          break;
      }

      taskActive = false;

      if (status.reason() == TaskStatus::REASON_CONTAINER_LIMITATION_DISK) {
        ++metrics.tasks_disk_full;

        // Increment abnormal_termination metric counter in case the task
        // wasn't supposed to consume beyond its disk quota but still got
        // terminated because of disk overuse.
        if (flags.disk_use_limit >= DISK_PER_TASK) {
          ++metrics.abnormal_terminations;
        }

        break;
      }

      ++metrics.abnormal_terminations;
      break;
    case TASK_KILLED:
    case TASK_LOST:
    case TASK_ERROR:
    case TASK_DROPPED:
    case TASK_UNREACHABLE:
    case TASK_GONE:
    case TASK_GONE_BY_OPERATOR:
      if (flags.run_once) {
        driver->abort();
      }

      taskActive = false;

      if (status.reason() != TaskStatus::REASON_INVALID_OFFERS) {
        ++metrics.abnormal_terminations;
      }

      break;
    case TASK_STARTING:
    case TASK_RUNNING:
    case TASK_STAGING:
    case TASK_KILLING:
    case TASK_UNKNOWN:
      break;
    }
  }

private:
  const Flags flags;
  const FrameworkInfo frameworkInfo;
  const string role;

  int tasksLaunched;
  bool taskActive;

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
    Metrics(const DiskFullSchedulerProcess& _scheduler)
      : uptime_secs(
            string(FRAMEWORK_METRICS_PREFIX) + "/uptime_secs",
            defer(_scheduler, &DiskFullSchedulerProcess::_uptime_secs)),
        registered(
            string(FRAMEWORK_METRICS_PREFIX) + "/registered",
            defer(_scheduler, &DiskFullSchedulerProcess::_registered)),
        tasks_finished(string(FRAMEWORK_METRICS_PREFIX) + "/tasks_finished"),
        tasks_disk_full(string(FRAMEWORK_METRICS_PREFIX) + "/tasks_disk_full"),
        abnormal_terminations(
            string(FRAMEWORK_METRICS_PREFIX) + "/abnormal_terminations")
    {
      process::metrics::add(uptime_secs);
      process::metrics::add(registered);
      process::metrics::add(tasks_finished);
      process::metrics::add(tasks_disk_full);
      process::metrics::add(abnormal_terminations);
    }

    ~Metrics()
    {
      process::metrics::remove(uptime_secs);
      process::metrics::remove(registered);
      process::metrics::remove(tasks_finished);
      process::metrics::remove(tasks_disk_full);
      process::metrics::remove(abnormal_terminations);
    }

    process::metrics::PullGauge uptime_secs;
    process::metrics::PullGauge registered;

    process::metrics::Counter tasks_finished;
    process::metrics::Counter tasks_disk_full;
    process::metrics::Counter abnormal_terminations;
  } metrics;
};


// This scheduler starts a task which gradually consumes the disk until it
// reaches the limit. The framework expects the disk quota to be enforced
// and the task to be killed.
class DiskFullScheduler : public Scheduler
{
public:
  DiskFullScheduler(const Flags& _flags, const FrameworkInfo& _frameworkInfo)
    : process(_flags, _frameworkInfo)
  {
    process::spawn(process);
  }

  ~DiskFullScheduler() override
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

    process::dispatch(&process, &DiskFullSchedulerProcess::registered);
  }

  void reregistered(SchedulerDriver*, const MasterInfo&) override
  {
    LOG(INFO) << "Reregistered";

    process::dispatch(&process, &DiskFullSchedulerProcess::registered);
  }

  void disconnected(SchedulerDriver*) override
  {
    LOG(INFO) << "Disconnected";

    process::dispatch(
        &process,
        &DiskFullSchedulerProcess::disconnected);
  }

  void resourceOffers(
      SchedulerDriver* driver,
      const std::vector<Offer>& offers) override
  {
    LOG(INFO) << "Resource offers received";

    process::dispatch(
         &process,
         &DiskFullSchedulerProcess::resourceOffers,
         driver,
         offers);
  }

  void offerRescinded(SchedulerDriver*, const OfferID&) override
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
        &DiskFullSchedulerProcess::statusUpdate,
        driver,
        status);
  }

  void frameworkMessage(
      SchedulerDriver*,
      const ExecutorID&,
      const SlaveID&,
      const string& data) override
  {
    LOG(INFO) << "Framework message: " << data;
  }

  void slaveLost(SchedulerDriver*, const SlaveID& slaveId) override
  {
    LOG(INFO) << "Agent lost: " << slaveId;
  }

  void executorLost(
      SchedulerDriver*,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      int) override
  {
    LOG(INFO) << "Executor '" << executorId << "' lost on agent: " << slaveId;
  }

  void error(SchedulerDriver*, const string& message) override
  {
    LOG(INFO) << "Error message: " << message;
  }

private:
  DiskFullSchedulerProcess process;
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

  internal::logging::initialize(argv[0], false);

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  FrameworkInfo framework;
  framework.set_user(""); // Have Mesos fill the current user.
  framework.set_principal(flags.principal);
  framework.set_name(flags.name);
  framework.set_checkpoint(flags.checkpoint);
  framework.add_roles(flags.role);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  DiskFullScheduler scheduler(flags, framework);

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
