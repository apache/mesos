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

#include <boost/lexical_cast.hpp>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>
#include <mesos/v1/scheduler.hpp>

#include <mesos/authorizer/acls.hpp>

#include <process/delay.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/exit.hpp>
#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>

#include <stout/os/realpath.hpp>

#include "common/status_utils.hpp"

#include "examples/flags.hpp"

#include "logging/logging.hpp"

using namespace mesos::v1;

using std::cerr;
using std::cout;
using std::endl;
using std::queue;
using std::string;
using std::vector;

using boost::lexical_cast;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;

const int32_t CPUS_PER_TASK = 1;
const int32_t MEM_PER_TASK = 128;

constexpr char EXECUTOR_BINARY[] = "test-http-executor";
constexpr char EXECUTOR_NAME[] = "Test Executor (C++)";
constexpr char FRAMEWORK_NAME[] = "Event Call Scheduler using libprocess (C++)";


class HTTPScheduler : public process::Process<HTTPScheduler>
{
public:
  HTTPScheduler(const FrameworkInfo& _framework,
                const ExecutorInfo& _executor,
                const string& _master,
                const Option<Credential>& _credential)
    : framework(_framework),
      role(_framework.roles(0)),
      executor(_executor),
      master(_master),
      credential(_credential),
      state(INITIALIZING),
      tasksLaunched(0),
      tasksFinished(0),
      totalTasks(5) {}

  ~HTTPScheduler() override {}

  void connected()
  {
    doReliableRegistration();
  }

  void disconnected()
  {
    state = DISCONNECTED;
  }

  void received(queue<Event> events)
  {
    while (!events.empty()) {
      Event event = events.front();
      events.pop();

      switch (event.type()) {
        case Event::SUBSCRIBED: {
          cout << endl << "Received a SUBSCRIBED event" << endl;

          framework.mutable_id()->CopyFrom(event.subscribed().framework_id());
          state = SUBSCRIBED;

          cout << "Subscribed with ID " << framework.id() << endl;
          break;
        }

        case Event::OFFERS: {
          cout << endl << "Received an OFFERS event" << endl;
          resourceOffers(google::protobuf::convert(event.offers().offers()));
          break;
        }

        case Event::INVERSE_OFFERS: {
          cout << endl << "Received an INVERSE_OFFERS event" << endl;
          break;
        }

        case Event::RESCIND: {
          cout << endl << "Received a RESCIND event" << endl;
          break;
        }

        case Event::RESCIND_INVERSE_OFFER: {
          cout << endl << "Received a RESCIND_INVERSE_OFFER event" << endl;
          break;
        }

        case Event::UPDATE: {
          cout << endl << "Received an UPDATE event" << endl;

          // TODO(zuyu): Do batch processing of UPDATE events.
          statusUpdate(event.update().status());
          break;
        }

        // TODO(greggomann): Implement handling of operation status updates.
        case Event::UPDATE_OPERATION_STATUS:
          break;

        case Event::MESSAGE: {
          cout << endl << "Received a MESSAGE event" << endl;
          break;
        }

        case Event::FAILURE: {
          cout << endl << "Received a FAILURE event" << endl;

          if (event.failure().has_executor_id()) {
            // Executor failed.
            cout << "Executor '"
                 << event.failure().executor_id().value() << "' terminated";

            if (event.failure().has_agent_id()) {
              cout << " on Agent '"
                   << event.failure().agent_id().value() << "'";
            }

            if (event.failure().has_status()) {
              cout << ", and " << WSTRINGIFY(event.failure().status());
            }

            cout << endl;
          } else if (event.failure().has_agent_id()) {
            // Agent failed.
            cout << "Agent '" << event.failure().agent_id().value()
                 << "' terminated" << endl;
          }
          break;
        }

        case Event::ERROR: {
          cout << endl << "Received an ERROR event: "
               << event.error().message() << endl;
          process::terminate(self());
          break;
        }

        case Event::HEARTBEAT: {
          cout << endl << "Received a HEARTBEAT event" << endl;
          break;
        }

        case Event::UNKNOWN: {
          LOG(WARNING) << "Received an UNKNOWN event and ignored";
          break;
        }
      }
    }
  }

protected:
  void initialize() override
  {
    // We initialize the library here to ensure that callbacks are only invoked
    // after the process has spawned.
    mesos.reset(
        new scheduler::Mesos(
            master,
            mesos::ContentType::PROTOBUF,
            process::defer(self(), &Self::connected),
            process::defer(self(), &Self::disconnected),
            process::defer(self(), &Self::received, lambda::_1),
            credential));
  }

private:
  void resourceOffers(const vector<Offer>& offers)
  {
    foreach (const Offer& offer, offers) {
      cout << "Received offer " << offer.id() << " with "
           << Resources(offer.resources()) << endl;

      Resources taskResources = Resources::parse(
          "cpus:" + stringify(CPUS_PER_TASK) +
          ";mem:" + stringify(MEM_PER_TASK)).get();
      taskResources.allocate(role);

      Resources remaining = offer.resources();

      // Launch tasks.
      vector<TaskInfo> tasks;
      while (tasksLaunched < totalTasks &&
             remaining.toUnreserved().contains(taskResources)) {
        int taskId = tasksLaunched++;

        cout << "Launching task " << taskId << " using offer "
             << offer.id() << endl;

        TaskInfo task;
        task.set_name("Task " + lexical_cast<string>(taskId));
        task.mutable_task_id()->set_value(lexical_cast<string>(taskId));
        task.mutable_agent_id()->MergeFrom(offer.agent_id());
        task.mutable_executor()->MergeFrom(executor);

        Option<Resources> resources = [&]() {
          if (role == "*") {
            return remaining.find(taskResources);
          } else {
            Resource::ReservationInfo reservation;
            reservation.set_type(Resource::ReservationInfo::STATIC);
            reservation.set_role(role);

            return remaining.find(taskResources.pushReservation(reservation));
          }
        }();

        CHECK_SOME(resources);

        task.mutable_resources()->CopyFrom(resources.get());

        remaining -= resources.get();

        tasks.push_back(task);
      }

      Call call;
      CHECK(framework.has_id());
      call.mutable_framework_id()->CopyFrom(framework.id());
      call.set_type(Call::ACCEPT);

      Call::Accept* accept = call.mutable_accept();
      accept->add_offer_ids()->CopyFrom(offer.id());

      Offer::Operation* operation = accept->add_operations();
      operation->set_type(Offer::Operation::LAUNCH);
      foreach (const TaskInfo& taskInfo, tasks) {
        operation->mutable_launch()->add_task_infos()->CopyFrom(taskInfo);
      }

      mesos->send(call);
    }
  }

  void statusUpdate(const TaskStatus& status)
  {
    cout << "Task " << status.task_id() << " is in state " << status.state();

    if (status.has_message()) {
      cout << " with message '" << status.message() << "'";
    }
    cout << endl;

    if (status.has_uuid()) {
      Call call;
      CHECK(framework.has_id());
      call.mutable_framework_id()->CopyFrom(framework.id());
      call.set_type(Call::ACKNOWLEDGE);

      Call::Acknowledge* ack = call.mutable_acknowledge();
      ack->mutable_agent_id()->CopyFrom(status.agent_id());
      ack->mutable_task_id()->CopyFrom(status.task_id());
      ack->set_uuid(status.uuid());

      mesos->send(call);
    }

    if (status.state() == TASK_FINISHED) {
      ++tasksFinished;
    }

    if (status.state() == TASK_LOST ||
        status.state() == TASK_KILLED ||
        status.state() == TASK_FAILED) {
      EXIT(EXIT_FAILURE)
        << "Exiting because task " << status.task_id()
        << " is in unexpected state " << status.state()
        << " with reason " << status.reason()
        << " from source " << status.source()
        << " with message '" << status.message() << "'";
    }

    if (tasksFinished == totalTasks) {
      process::terminate(self());
    }
  }

  void doReliableRegistration()
  {
    if (state == SUBSCRIBED) {
      return;
    }

    Call call;
    if (framework.has_id()) {
      call.mutable_framework_id()->CopyFrom(framework.id());
    }
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(framework);

    mesos->send(call);

    process::delay(Seconds(1), self(), &Self::doReliableRegistration);
  }

  void finalize() override
  {
    Call call;
    CHECK(framework.has_id());
    call.mutable_framework_id()->CopyFrom(framework.id());
    call.set_type(Call::TEARDOWN);

    mesos->send(call);
  }

  FrameworkInfo framework;
  const string role;
  const ExecutorInfo executor;
  const string master;
  const Option<Credential> credential;
  process::Owned<scheduler::Mesos> mesos;

  enum State
  {
    INITIALIZING = 0,
    SUBSCRIBED = 1,
    DISCONNECTED = 2
  } state;

  int tasksLaunched;
  int tasksFinished;
  const int totalTasks;
};


void usage(const char* argv0, const flags::FlagsBase& flags)
{
  cerr << "Usage: " << Path(argv0).basename() << " [...]" << endl
       << endl
       << "Supported options:" << endl
       << flags.usage();
}


class Flags : public virtual mesos::internal::examples::Flags {};


int main(int argc, char** argv)
{
  // Find this executable's directory to locate executor.
  string uri;
  Option<string> value = os::getenv("MESOS_HELPER_DIR");
  if (value.isSome()) {
    uri = path::join(value.get(), EXECUTOR_BINARY);
  } else {
    uri =
      path::join(os::realpath(Path(argv[0]).dirname()).get(), EXECUTOR_BINARY);
  }

  Flags flags;

  Try<flags::Warnings> load = flags.load("MESOS_EXAMPLE_", argc, argv);

  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  mesos::internal::logging::initialize(argv[0], true, flags); // Catch signals.

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  FrameworkInfo framework;
  framework.set_principal(flags.principal);
  framework.set_name(FRAMEWORK_NAME);
  framework.set_checkpoint(flags.checkpoint);
  framework.add_roles(flags.role);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  const Result<string> user = os::user();

  CHECK_SOME(user);
  framework.set_user(user.get());

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.mutable_command()->set_value(uri);
  executor.set_name(EXECUTOR_NAME);

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
    acl->mutable_roles()->add_values("*");
    os::setenv("MESOS_ACLS", stringify(JSON::protobuf(acls)));
  }

  process::Owned<HTTPScheduler> scheduler(
      new HTTPScheduler(framework, executor, flags.master, credential));

  process::spawn(scheduler.get());
  process::wait(scheduler.get());

  return EXIT_SUCCESS;
}
