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

#include <mesos/scheduler.hpp>

#include <mesos/authorizer/acls.hpp>

#include <process/defer.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/stopwatch.hpp>
#include <stout/strings.hpp>

#include "examples/flags.hpp"

#include "logging/logging.hpp"

using namespace mesos;
using namespace process;

using std::string;
using std::vector;

constexpr char FRAMEWORK_NAME[] = "Load Generator Framework (C++)";


// Generate load towards the master (by repeatedly sending
// ReconcileTasksMessages) at the specified rate and for the
// specified duration.
class LoadGeneratorProcess : public Process<LoadGeneratorProcess>
{
public:
  LoadGeneratorProcess(
      SchedulerDriver* _driver,
      double _qps,
      const Option<Duration>& _duration)
    : driver(_driver), qps(_qps), duration(_duration), messages(0) {}

  void initialize() override
  {
    dispatch(self(), &Self::generate);
  }

private:
  void generate()
  {
    watch.start();

    while (true) {
      Duration elapsed = watch.elapsed();

      if (duration.isSome() && elapsed >= duration.get()) {
        LOG(INFO) << "LoadGenerator generated " << messages
                  << " messages in " << elapsed << " (throughput = "
                  << (messages / elapsed.secs()) << " messages/sec)";

        LOG(INFO) << "Stopping LoadGenerator and scheduler driver";

        terminate(self());
        driver->stop();
        return;
      }

      Stopwatch reconcile;
      reconcile.start();

      driver->reconcileTasks(vector<TaskStatus>());

      messages++;

      // Compensate for the driver call overhead.
      os::sleep(std::max(
          Duration::zero(),
          Seconds(1) / qps - reconcile.elapsed()));
    }
  }

  SchedulerDriver* driver;
  double qps;
  const Option<Duration> duration;
  int messages;
  Stopwatch watch;
};


class LoadGenerator
{
public:
  LoadGenerator(
      SchedulerDriver* driver,
      double qps,
      const Option<Duration>& duration)
  {
    process = new LoadGeneratorProcess(driver, qps, duration);
    spawn(process);
  }

  ~LoadGenerator()
  {
    // Could be already terminated.
    terminate(process);
    wait(process);
    delete process;
  }

private:
  LoadGeneratorProcess* process;
};


// This scheduler does one thing: generating network traffic towards
// the master.
class LoadGeneratorScheduler : public Scheduler
{
public:
  LoadGeneratorScheduler(double _qps, const Option<Duration>& _duration)
    : generator(nullptr), qps(_qps), duration(_duration) {}

  ~LoadGeneratorScheduler() override
  {
    delete generator;
  }

  void registered(SchedulerDriver* driver,
                          const FrameworkID&,
                          const MasterInfo& masterInfo) override
  {
    LOG(INFO) << "Registered with " << masterInfo.pid();

    if (generator == nullptr) {
      LOG(INFO) << "Starting LoadGenerator at QPS: " << qps;

      generator = new LoadGenerator(driver, qps, duration);
    }
  }

  void reregistered(
      SchedulerDriver* driver,
      const MasterInfo& masterInfo) override
  {
    LOG(INFO) << "Reregistered with " << masterInfo.pid();

    if (generator == nullptr) {
      LOG(INFO) << "Starting LoadGenerator at QPS: " << qps;
      generator = new LoadGenerator(driver, qps, duration);
    }
  }

  void disconnected(SchedulerDriver* driver) override
  {
    LOG(INFO) << "Disconnected!";

    delete generator;
    generator = nullptr;

    LOG(INFO) << "Stopped LoadGenerator";
  }

  void resourceOffers(
      SchedulerDriver* driver,
      const vector<Offer>& offers) override
  {
    LOG(INFO) << "Received " << offers.size()
              << " resource offers. Declining them";

    Filters filters;

    // Refuse for eternity so Master doesn't send us the same
    // offers.
    filters.set_refuse_seconds(Duration::max().secs());

    for (size_t i = 0; i < offers.size(); i++) {
      driver->declineOffer(offers[i].id(), filters);
    }
  }

  void offerRescinded(SchedulerDriver*, const OfferID&) override {}

  void statusUpdate(SchedulerDriver*, const TaskStatus&) override {}

  void frameworkMessage(
      SchedulerDriver*,
      const ExecutorID&,
      const SlaveID&,
      const string&) override {}

  void slaveLost(SchedulerDriver*, const SlaveID&) override {}

  void executorLost(
      SchedulerDriver*,
      const ExecutorID&,
      const SlaveID&,
      int) override {}

  void error(SchedulerDriver*, const string& error) override
  {
    // Terminating process with EXIT here because we cannot interrupt
    // LoadGenerator's long-running loop.
    EXIT(EXIT_FAILURE) << "Error received: " << error;
  }

private:
  LoadGenerator* generator;
  const double qps;
  const Option<Duration> duration;
};


class Flags : public virtual mesos::internal::examples::Flags
{
public:
  Flags()
  {
    add(&Flags::qps,
        "qps",
        "Required. Generate load at this specified rate (queries per second).\n"
        "Note that this rate is an upper bound and the real rate may be less.\n"
        "Also, setting the qps too high can cause the local machine to run\n"
        "out of ephemeral ports during master failover (if scheduler driver\n"
        "fails to detect master change soon enough after the old master exits\n"
        "and the scheduler keeps trying to connect to the dead master. See\n"
        "MESOS-1560 for more details)");

    add(&Flags::duration,
        "duration",
        "Run LoadGenerator for the specified duration.\n"
        "Without this option this framework would keep generating load\n"
        "forever as long as it is connected to the master");
  }

  double qps;
  Option<Duration> duration;
};


int main(int argc, char** argv)
{
  Flags flags;
  Try<flags::Warnings> load = flags.load("MESOS_EXAMPLE_", argc, argv);

  if (load.isError()) {
    std::cerr << flags.usage(load.error()) << std::endl;
    return EXIT_FAILURE;
  }

  if (flags.help) {
    std::cout << flags.usage() << std::endl;
    return EXIT_SUCCESS;
  }

  mesos::internal::logging::initialize(argv[0], false);

  // Log any flag warnings (after logging is initialized).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  if (flags.qps <= 0.0) {
    EXIT(EXIT_FAILURE) << "Flag '--qps' needs to be greater than zero";
  }

  LoadGeneratorScheduler scheduler(flags.qps, flags.duration);

  FrameworkInfo framework;
  framework.set_user(""); // Have Mesos fill in the current user.
  framework.set_principal(flags.principal);
  framework.set_name(FRAMEWORK_NAME);
  framework.set_checkpoint(flags.checkpoint);
  framework.add_roles(flags.role);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);
  framework.set_checkpoint(flags.checkpoint);

  if (flags.master == "local") {
    // Configure master.
    os::setenv("MESOS_ROLES", flags.role);
    os::setenv("MESOS_AUTHENTICATE_FRAMEWORKS", stringify(flags.authenticate));

    ACLs acls;
    ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->set_type(ACL::Entity::ANY);
    acl->mutable_roles()->add_values("*");
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

  int status = driver->run() == DRIVER_STOPPED ? EXIT_SUCCESS : EXIT_FAILURE;

  // Ensure that the driver process terminates.
  driver->stop();

  delete driver;
  return status;
}
