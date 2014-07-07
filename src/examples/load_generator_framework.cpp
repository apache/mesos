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

#include <iostream>
#include <string>

#include <process/defer.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

#include <mesos/scheduler.hpp>

#include <stout/os.hpp>
#include <stout/stopwatch.hpp>

#include "logging/flags.hpp"
#include "logging/logging.hpp"

using namespace mesos;
using namespace process;

using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::vector;


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

  virtual void initialize()
  {
    dispatch(self(), &Self::generate);
  }

private:
  void generate()
  {
    watch.start();

    while (true) {
      Duration elapsed =  watch.elapsed();

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
    : generator(NULL), qps(_qps), duration(_duration) {}

  virtual ~LoadGeneratorScheduler()
  {
    delete generator;
  }

  virtual void registered(SchedulerDriver* driver,
                          const FrameworkID&,
                          const MasterInfo& masterInfo)
  {
    LOG(INFO) << "Registered with " << masterInfo.pid();

    if (generator == NULL) {
      LOG(INFO) << "Starting LoadGenerator at QPS: " << qps;

      generator = new LoadGenerator(driver, qps, duration);
    }
  }

  virtual void reregistered(
      SchedulerDriver* driver,
      const MasterInfo& masterInfo)
  {
    LOG(INFO) << "Reregistered with " << masterInfo.pid();

    if (generator == NULL) {
      LOG(INFO) << "Starting LoadGenerator at QPS: " << qps;
      generator = new LoadGenerator(driver, qps, duration);
    }
  }

  virtual void disconnected(SchedulerDriver* driver)
  {
    LOG(INFO) << "Disconnected!";

    delete generator;
    generator = NULL;

    LOG(INFO) << "Stopped LoadGenerator";
  }

  virtual void resourceOffers(
      SchedulerDriver* driver,
      const vector<Offer>& offers)
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

  virtual void offerRescinded(SchedulerDriver*, const OfferID&) {}

  virtual void statusUpdate(SchedulerDriver*, const TaskStatus&) {}

  virtual void frameworkMessage(
      SchedulerDriver*,
      const ExecutorID&,
      const SlaveID&,
      const string&) {}

  virtual void slaveLost(SchedulerDriver*, const SlaveID&) {}

  virtual void executorLost(
      SchedulerDriver*,
      const ExecutorID&,
      const SlaveID&,
      int) {}

  virtual void error(SchedulerDriver*, const string&) {}

private:
  LoadGenerator* generator;
  const double qps;
  const Option<Duration> duration;
};


class Flags : public mesos::internal::logging::Flags
{
public:
  Flags()
  {
    add(&master,
        "master",
        "Required. The master to connect to. May be one of:\n"
        "  master@addr:port (The PID of the master)\n"
        "  zk://host1:port1,host2:port2,.../path\n"
        "  zk://username:password@host1:port1,host2:port2,.../path\n"
        "  file://path/to/file (where file contains one of the above)");

    add(&Flags::authenticate,
        "authenticate",
        "Set to 'true' to enable framework authentication",
        false);

    add(&Flags::principal,
        "principal",
        "The principal used to identify this framework",
        "load-generator-framework");

    add(&Flags::secret,
        "secret",
        "The secret used to authenticate this framework");

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

    add(&Flags::help,
        "help",
        "Print this help message",
        false);
  }

  Option<string> master;
  string principal;
  Option<string> secret;
  bool authenticate;
  bool help;
  Option<double> qps;
  Option<Duration> duration;
};


void usage(const char* argv0, const flags::FlagsBase& flags)
{
  cerr << "Usage: " << os::basename(argv0).get() << " [...]" << endl
       << endl
       << "Supported options:" << endl
       << flags.usage();
}


int main(int argc, char** argv)
{
  Flags flags;

  Try<Nothing> load = flags.load("MESOS_", argc, argv);

  if (load.isError()) {
    cerr << load.error() << endl;
    usage(argv[0], flags);
    exit(1);
  }

  if (flags.help) {
    usage(argv[0], flags);
    exit(1);
  }

  if (flags.master.isNone()) {
    EXIT(1) << "Missing required option --master. See --help";
  }

  if (flags.qps.isNone()) {
    EXIT(1) << "Missing required option --qps. See --help";
  }

  if (flags.qps.get() <= 0) {
    EXIT(1) << "--qps needs to be greater than zero";
  }

  // We want the logger to catch failure signals.
  mesos::internal::logging::initialize(argv[0], flags, true);

  LoadGeneratorScheduler scheduler(flags.qps.get(), flags.duration);

  FrameworkInfo framework;
  framework.set_user(""); // Have Mesos fill in the current user.
  framework.set_name("Load Generator Framework (C++)");

  MesosSchedulerDriver* driver;
  if (flags.authenticate) {
    cout << "Enabling authentication for the framework" << endl;

    if (flags.secret.isNone()) {
      EXIT(1) << "Expecting --secret when --authenticate is set";
    }

    Credential credential;
    credential.set_principal(flags.principal);
    credential.set_secret(flags.secret.get());

    framework.set_principal(flags.principal);

    driver = new MesosSchedulerDriver(
        &scheduler, framework, flags.master.get(), credential);
  } else {
    framework.set_principal(flags.principal);

    driver = new MesosSchedulerDriver(
        &scheduler, framework, flags.master.get());
  }

  int status = driver->run() == DRIVER_STOPPED ? 0 : 1;

  // Ensure that the driver process terminates.
  driver->stop();

  delete driver;
  return status;
}
