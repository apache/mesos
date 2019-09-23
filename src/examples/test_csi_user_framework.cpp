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

#include <mesos/authorizer/acls.hpp>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>
#include <mesos/v1/scheduler.hpp>

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
#include <stout/uuid.hpp>

#include "common/status_utils.hpp"

#include "examples/flags.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

using namespace mesos::v1;

using std::cerr;
using std::cout;
using std::endl;
using std::queue;
using std::string;
using std::vector;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;

constexpr Seconds REFUSE_TIME = Seconds::max();

constexpr char FRAMEWORK_NAME[] =
  "CSI User Event Call Scheduler using libprocess (C++)";


/**
 * Example framework performing CSI-related operations.
 *
 * This class implements a framework communicating with Mesos over the v1 HTTP
 * API which transform certain disk resources using CSI operations.
 *
 * It expects to be offered disk resources from resource providers reserved for
 * its role (which likely should not be shared by any other framework in the
 * cluster).  It will then convert `RAW` disk resources into `MOUNT` volumes and
 * unreserve these `MOUNT` resources when they are offered again so that
 * frameworks in other roles can consume the created disk resource.
 *
 * The bulk of the framework logic is implemented in `resourceOffers`.
 */
class HTTPScheduler : public process::Process<HTTPScheduler>
{
public:
  HTTPScheduler(
      const FrameworkInfo& _framework,
      const string& _master,
      const Option<Credential>& _credential)
    : framework(_framework),
      master(_master),
      credential(_credential),
      state(INITIALIZING) {}

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
          cout << "Unexpected UPDATE event: " << event.update().DebugString()
               << endl;
          process::terminate(self());
          break;
        }

        // TODO(greggomann): Implement handling of offer operation updates.
        case Event::UPDATE_OPERATION_STATUS:
          break;

        case Event::MESSAGE: {
          cout << endl << "Received a MESSAGE event" << endl;
          break;
        }

        case Event::FAILURE: {
          cout << endl << "Received a FAILURE event" << endl;

          if (event.failure().has_agent_id()) {
            // Agent failed.
            cout << "Agent '" << event.failure().agent_id().value()
                 << "' terminated" << endl;
          } else {
            cout << "Unexpected FAILURE event: "
                 << event.failure().DebugString() << endl;
            process::terminate(self());
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

      // Filter resources and group interesting resources by resource provider.
      //
      // NOTE: We introduce a typedef here so we can use this type
      // with the `foreachvalue` preprocessor macro below.
      using ResourcesByType =
        hashmap<Resource::DiskInfo::Source::Type, Resources>;

      hashmap<ResourceProviderID, ResourcesByType> resourcesByProvider;

      constexpr Resource::DiskInfo::Source::Type RAW =
        Resource::DiskInfo::Source::RAW;

      constexpr Resource::DiskInfo::Source::Type MOUNT =
        Resource::DiskInfo::Source::MOUNT;

      foreach(const Resource& resource, offer.resources()) {
        // Ignore any resources not from a resource provider.
        if (!resource.has_provider_id()) {
          continue;
        }

        // We only convert reserved resources.
        if (!Resources::isReserved(resource)) {
          continue;
        }

        // We either work on `RAW` or `MOUNT` disk resources. Store
        // them by type and ignore any other resources.
        if (Resources::isDisk(resource, RAW)) {
          resourcesByProvider[resource.provider_id()][RAW] += resource;
        } else if (Resources::isDisk(resource, MOUNT)) {
          resourcesByProvider[resource.provider_id()][MOUNT] += resource;
        }
      }

      // Create operations.

      // Create a single call which is either an accept and contains
      // at least one operation, or a decline.
      Call call;
      CHECK(framework.has_id());
      call.mutable_framework_id()->CopyFrom(framework.id());

      // We create operations on resources from each resource provider
      // separately as most operations currently cannot operate on multiple
      // resource providers at once (`LAUNCH` being the obvious exception).
      foreachvalue (
          const ResourcesByType& resourcesByType,
          resourcesByProvider) {
        // Iterate over disk resources grouped by disk type since the
        // performed operation depends on the type.
        foreachpair (
            const Resource::DiskInfo::Source::Type& type,
            const Resources& resources,
            resourcesByType) {
          call.set_type(Call::ACCEPT);
          Call::Accept* accept = call.mutable_accept();
          if (accept->offer_ids().empty()) {
            accept->add_offer_ids()->CopyFrom(offer.id());
          }

          if (type == RAW) {
            // We create `MOUNT` volumes out of `RAW` disk resources.
            foreach (const Resource& resource, resources) {
              cout << "Converting 'RAW' disk to 'MOUNT' disk" << endl;

              Offer::Operation* operation = accept->add_operations();
              operation->set_type(Offer::Operation::CREATE_DISK);

              Offer::Operation::CreateDisk* createDisk =
                operation->mutable_create_disk();

              createDisk->mutable_source()->CopyFrom(resource);
              createDisk->set_target_type(
                  Resource::DiskInfo::Source::MOUNT);
            }
          } else if (type == MOUNT) {
            // We unreserve `MOUNT` disk resources so they can be
            // consumed by frameworks in other roles.
            foreach (const Resource& resource, resources) {
              cout << "Unreserving 'MOUNT' disk" << endl;

              Offer::Operation* operation = accept->add_operations();
              operation->set_type(Offer::Operation::UNRESERVE);

              Offer::Operation::Unreserve* unreserve =
                operation->mutable_unreserve();

              unreserve->add_resources()->CopyFrom(resource);
            }
          }
        }
      }

      // If we did not create operations to accept the offer with decline it.
      if (!call.has_accept() || call.accept().operations().empty()) {
        cout << "Declining offer" << endl;

        call.clear_accept();
        call.set_type(Call::DECLINE);
        Call::Decline* decline = call.mutable_decline();
        decline->add_offer_ids()->CopyFrom(offer.id());
        decline->mutable_filters()->set_refuse_seconds(REFUSE_TIME.secs());
      }

      mesos->send(call);
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

  FrameworkInfo framework;
  string master;
  Option<Credential> credential;
  process::Owned<scheduler::Mesos> mesos;

  enum State
  {
    INITIALIZING = 0,
    SUBSCRIBED = 1,
    DISCONNECTED = 2
  } state;
};


class Flags : public virtual mesos::internal::examples::Flags,
              public virtual mesos::internal::logging::Flags
{};


int main(int argc, char** argv)
{
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
    os::setenv(
        "MESOS_AUTHENTICATE_HTTP_FRAMEWORKS", stringify(flags.authenticate));

    os::setenv("MESOS_HTTP_FRAMEWORK_AUTHENTICATORS", "basic");

    mesos::ACLs acls;
    mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
    acl->mutable_principals()->set_type(mesos::ACL::Entity::ANY);
    acl->mutable_roles()->add_values("*");
    os::setenv("MESOS_ACLS", stringify(JSON::protobuf(acls)));
  }

  process::Owned<HTTPScheduler> scheduler(
      new HTTPScheduler(framework, flags.master, credential));

  process::spawn(scheduler.get());
  process::wait(scheduler.get());

  return EXIT_SUCCESS;
}
