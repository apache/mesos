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

#include <libgen.h>

#include <iostream>
#include <string>
#include <queue>

#include <boost/lexical_cast.hpp>

#include <process/delay.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/exit.hpp>
#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>

#include "logging/flags.hpp"

using namespace mesos;

using std::cerr;
using std::cout;
using std::endl;
using std::queue;
using std::string;
using std::vector;

using boost::lexical_cast;

using mesos::Resources;
using mesos::scheduler::Call;
using mesos::scheduler::Event;

const int32_t CPUS_PER_TASK = 1;
const int32_t MEM_PER_TASK = 32;

class LowLevelScheduler : public process::Process<LowLevelScheduler>
{
public:
  LowLevelScheduler(const FrameworkInfo& _framework,
                    const ExecutorInfo& _executor,
                    const string& _master)
    : framework(_framework),
      executor(_executor),
      mesos(_master,
            process::defer(self(), &Self::connected),
            process::defer(self(), &Self::disconnected),
            process::defer(self(), &Self::received, lambda::_1)),
      state(INITIALIZING),
      tasksLaunched(0),
      tasksFinished(0),
      totalTasks(5) {}

  LowLevelScheduler(const FrameworkInfo& _framework,
                    const ExecutorInfo& _executor,
                    const string& _master,
                    const Credential& credential)
    : framework(_framework),
      executor(_executor),
      mesos(_master,
            credential,
            process::defer(self(), &Self::connected),
            process::defer(self(), &Self::disconnected),
            process::defer(self(), &Self::received, lambda::_1)),
      state(INITIALIZING),
      tasksLaunched(0),
      tasksFinished(0),
      totalTasks(5) {}

  ~LowLevelScheduler() {}

  void connected(void)
  {
    doReliableRegistration();
  }

  void disconnected(void)
  {
    state = DISCONNECTED;
  }

  void received(queue<Event> events)
  {
    while (!events.empty()) {
      Event event = events.front();
      events.pop();

      switch (event.type()) {
        case Event::REGISTERED: {
          cout << endl << "Received a REGISTERED event" << endl;

          framework.mutable_id()->CopyFrom(
              event.registered().framework_id());
          state = REGISTERED;

          cout << "Framework '" << event.registered().framework_id().value()
               << "' registered with Master '"
               << event.registered().master_info().id() << "'" << endl;
          break;
        }

        case Event::REREGISTERED: {
          cout << endl << "Received a REREGISTERED event" << endl;

          state = REGISTERED;

          cout << "Framework '" << event.reregistered().framework_id().value()
               << "' re-registered to Master '"
               << event.reregistered().master_info().id() << "'" << endl;
          break;
        }

        case Event::OFFERS: {
          cout << endl << "Received an OFFERS event" << endl;
          resourceOffers(google::protobuf::convert(event.offers().offers()));
          break;
        }

        case Event::RESCIND: {
          cout << endl << "Received a RESCIND event" << endl;
          break;
        }

        case Event::UPDATE: {
          cout << endl << "Received an UPDATE event" << endl;

          // TODO(zuyu): Do batch processing of UPDATE events.
          statusUpdate(event.update().uuid(), event.update().status());
          break;
        }

        case Event::MESSAGE: {
          cout << endl << "Received a MESSAGE event" << endl;
          break;
        }

        case Event::FAILURE: {
          cout << endl << "Received a FAILURE event" << endl;

          if (event.failure().has_executor_id()) {
            // Executor failure.
            cout << "Executor '"
                 << event.failure().executor_id().value() << "' terminated";

            if (event.failure().has_slave_id()) {
              cout << " on Slave '"
                   << event.failure().slave_id().value() << "'";
            }

            if (event.failure().has_status()) {
              int status = event.failure().status();
              if (WIFEXITED(status)) {
                cout << ", and exited with status "
                     << WEXITSTATUS(status);
              } else {
                cout << ", and terminated with signal "
                     << WTERMSIG(status);
              }
            }
            cout << endl;
          } else if (event.failure().has_slave_id()) {
            // Slave failure.
            cout << "Slave '" << event.failure().slave_id().value()
                 << "' terminated" << endl;
          }
          break;
        }

        case Event::ERROR: {
          cout << endl << "Received an ERROR event" << endl;
          cout << event.error().message() << endl;
          process::terminate(self());
          break;
        }

        default: {
          EXIT(1) << "Received an UNKNOWN event";
        }
      }
    }
  }

private:
  void resourceOffers(const vector<Offer>& offers)
  {
    foreach (const Offer& offer, offers) {
      cout << "Offer '" << offer.id().value() << "' has "
           << offer.resources() << endl;

      static const Resources TASK_RESOURCES = Resources::parse(
          "cpus:" + stringify(CPUS_PER_TASK) +
          ";mem:" + stringify(MEM_PER_TASK)).get();

      Resources remaining = offer.resources();

      // Launch tasks.
      vector<TaskInfo> tasks;
      while (tasksLaunched < totalTasks &&
             TASK_RESOURCES <= remaining.flatten()) {
        int taskId = tasksLaunched++;

        cout << "Starting task " << taskId << " on "
             << offer.hostname() << endl;

        TaskInfo task;
        task.set_name("Task " + lexical_cast<string>(taskId));
        task.mutable_task_id()->set_value(
            lexical_cast<string>(taskId));
        task.mutable_slave_id()->MergeFrom(offer.slave_id());
        task.mutable_executor()->MergeFrom(executor);

        Option<Resources> resources =
          remaining.find(TASK_RESOURCES, framework.role());

        CHECK_SOME(resources);
        task.mutable_resources()->MergeFrom(resources.get());
        remaining -= resources.get();

        tasks.push_back(task);
      }

      Call call;
      call.mutable_framework_info()->CopyFrom(framework);
      call.set_type(Call::LAUNCH);

      Call::Launch* launch = call.mutable_launch();
      foreach (const TaskInfo& taskInfo, tasks) {
        launch->add_task_infos()->CopyFrom(taskInfo);
      }
      launch->add_offer_ids()->CopyFrom(offer.id());

      mesos.send(call);
    }
  }

  void statusUpdate(const string& uuid, const TaskStatus& status)
  {
    cout << "Task " << status.task_id().value() << " is in state "
         << TaskState_Name(status.state());

    if (status.has_message()) {
      cout << " with message '" << status.message() << "'";
    }
    cout << endl;

    Call call;
    call.mutable_framework_info()->CopyFrom(framework);
    call.set_type(Call::ACKNOWLEDGE);

    Call::Acknowledge* ack = call.mutable_acknowledge();
    ack->mutable_slave_id()->CopyFrom(status.slave_id());
    ack->mutable_task_id ()->CopyFrom(status.task_id ());
    ack->set_uuid(uuid);

    mesos.send(call);

    if (status.state() == TASK_FINISHED) {
      ++tasksFinished;
    }

    if (tasksFinished == totalTasks) {
      process::terminate(self());
    }
  }

  void doReliableRegistration()
  {
    if (state == REGISTERED) {
      return;
    }

    Call call;
    call.mutable_framework_info()->CopyFrom(framework);

    if (state == INITIALIZING) {
      call.set_type(Call::REGISTER);
    } else {  // state is DISCONNECTED.
      call.set_type(Call::REREGISTER);
    }

    mesos.send(call);

    process::delay(Seconds(1),
                   self(),
                   &Self::doReliableRegistration);
  }

  void finalize(void)
  {
    Call call;
    call.mutable_framework_info()->CopyFrom(framework);
    call.set_type(Call::UNREGISTER);

    mesos.send(call);
  }

  FrameworkInfo framework;
  const ExecutorInfo executor;
  scheduler::Mesos mesos;

  enum State {
    INITIALIZING = 0,
    REGISTERED = 1,
    DISCONNECTED = 2
  } state;

  int tasksLaunched;
  int tasksFinished;
  const int totalTasks;
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
  // Find this executable's directory to locate executor.
  string path = os::realpath(dirname(argv[0])).get();
  string uri = path + "/test-executor";
  if (getenv("MESOS_BUILD_DIR")) {
    uri = string(getenv("MESOS_BUILD_DIR")) + "/src/test-executor";
  }

  mesos::internal::logging::Flags flags;

  string role;
  flags.add(&role,
            "role",
            "Role to use when registering",
            "*");

  Option<string> master;
  flags.add(&master,
            "master",
            "ip:port of master to connect");

  Try<Nothing> load = flags.load(None(), argc, argv);

  if (load.isError()) {
    cerr << load.error() << endl;
    usage(argv[0], flags);
    exit(1);
  } else if (master.isNone()) {
    cerr << "Missing --master" << endl;
    usage(argv[0], flags);
    exit(1);
  }

  FrameworkInfo framework;
  framework.set_user(""); // Have Mesos fill in the current user.
  framework.set_name("Low-Level Scheduler using libprocess (C++)");
  framework.set_role(role);

  // TODO(vinod): Make checkpointing the default when it is default
  // on the slave.
  if (os::hasenv("MESOS_CHECKPOINT")) {
    cout << "Enabling checkpoint for the framework" << endl;
    framework.set_checkpoint(true);
  }

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.mutable_command()->set_value(uri);
  executor.set_name("Test Executor (C++)");
  executor.set_source("cpp_test");

  process::initialize();

  LowLevelScheduler* scheduler;
  if (os::hasenv("MESOS_AUTHENTICATE")) {
    cout << "Enabling authentication for the scheduler" << endl;

    if (!os::hasenv("DEFAULT_PRINCIPAL")) {
      EXIT(1) << "Expecting authentication principal in the environment";
    }

    if (!os::hasenv("DEFAULT_SECRET")) {
      EXIT(1) << "Expecting authentication secret in the environment";
    }

    Credential credential;
    credential.set_principal(getenv("DEFAULT_PRINCIPAL"));
    credential.set_secret(getenv("DEFAULT_SECRET"));

    framework.set_principal(getenv("DEFAULT_PRINCIPAL"));

    scheduler =
      new LowLevelScheduler(framework, executor, master.get(), credential);
  } else {
    framework.set_principal("low-level-scheduler-cpp");

    scheduler =
      new LowLevelScheduler(framework, executor, master.get());
  }

  process::spawn(scheduler);
  process::wait(scheduler);
  delete scheduler;

  return 0;
}
