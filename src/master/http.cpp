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

#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/array.hpp>

#include <mesos/attributes.hpp>
#include <mesos/type_utils.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include <mesos/maintenance/maintenance.hpp>

#include <mesos/v1/master.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/help.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/base64.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/representation.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "common/build.hpp"
#include "common/http.hpp"
#include "common/protobuf_utils.hpp"

#include "internal/devolve.hpp"

#include "logging/logging.hpp"

#include "master/machine.hpp"
#include "master/maintenance.hpp"
#include "master/master.hpp"
#include "master/validation.hpp"

#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

#include "version/version.hpp"

using google::protobuf::RepeatedPtrField;

using process::AUTHENTICATION;
using process::AUTHORIZATION;
using process::Clock;
using process::DESCRIPTION;
using process::Failure;
using process::Future;
using process::HELP;
using process::TLDR;

using process::http::Accepted;
using process::http::BadRequest;
using process::http::Conflict;
using process::http::Forbidden;
using process::http::InternalServerError;
using process::http::MethodNotAllowed;
using process::http::NotFound;
using process::http::NotImplemented;
using process::http::NotAcceptable;
using process::http::OK;
using process::http::Pipe;
using process::http::ServiceUnavailable;
using process::http::TemporaryRedirect;
using process::http::UnsupportedMediaType;
using process::http::URL;

using process::metrics::internal::MetricsProcess;

using std::list;
using std::map;
using std::set;
using std::string;
using std::tie;
using std::tuple;
using std::vector;


namespace mesos {

static void json(
    JSON::StringWriter* writer, const FrameworkInfo::Capability& capability)
{
  writer->append(FrameworkInfo::Capability::Type_Name(capability.type()));
}


static void json(JSON::ObjectWriter* writer, const Offer& offer)
{
  writer->field("id", offer.id().value());
  writer->field("framework_id", offer.framework_id().value());
  writer->field("slave_id", offer.slave_id().value());
  writer->field("resources", Resources(offer.resources()));
}

namespace internal {
namespace master {

// Pull in model overrides from common.
using mesos::internal::model;

// Pull in definitions from process.
using process::http::Response;
using process::http::Request;
using process::Owned;


// The summary representation of `T` to support the `/state-summary` endpoint.
// e.g., `Summary<Slave>`.
template <typename T>
struct Summary : Representation<T>
{
  using Representation<T>::Representation;
};


// The full representation of `T` to support the `/state` endpoint.
// e.g., `Full<Slave>`.
template <typename T>
struct Full : Representation<T>
{
  using Representation<T>::Representation;
};


// Implementation of the `ObjectApprover` interface authorizing all objects.
class AcceptingObjectApprover : public ObjectApprover
{
public:
  virtual Try<bool> approved(
      const Option<ObjectApprover::Object>& object) const noexcept override
  {
    return true;
  }
};


static bool approveViewFrameworkInfo(
    const Owned<ObjectApprover>& frameworksApprover,
    const FrameworkInfo& frameworkInfo)
{
  ObjectApprover::Object object;
  object.framework_info = &frameworkInfo;

  Try<bool> approved = frameworksApprover->approved(object);
  if (approved.isError()) {
    LOG(WARNING) << "Error during FrameworkInfo authorization: "
                 << approved.error();
    // TODO(joerg84): Consider exposing these errors to the caller.
    return false;
  }
  return approved.get();
}


static bool approveViewTaskInfo(
    const Owned<ObjectApprover>& tasksApprover,
    const TaskInfo& taskInfo,
    const FrameworkInfo& frameworkInfo)
{
  ObjectApprover::Object object;
  object.task_info = &taskInfo;
  object.framework_info = &frameworkInfo;

  Try<bool> approved = tasksApprover->approved(object);
  if (approved.isError()) {
    LOG(WARNING) << "Error during TaskInfo authorization: " << approved.error();
    // TODO(joerg84): Consider exposing these errors to the caller.
    return false;
  }
  return approved.get();
}


static bool approveViewTask(
    const Owned<ObjectApprover>& tasksApprover,
    const Task& task,
    const FrameworkInfo& frameworkInfo)
{
  ObjectApprover::Object object;
  object.task = &task;
  object.framework_info = &frameworkInfo;

  Try<bool> approved = tasksApprover->approved(object);
  if (approved.isError()) {
    LOG(WARNING) << "Error during Task authorization: " << approved.error();
     // TODO(joerg84): Consider exposing these errors to the caller.
    return false;
  }
  return approved.get();
}


static bool approveViewExecutorInfo(
    const Owned<ObjectApprover>& executorsApprover,
    const ExecutorInfo& executorInfo,
    const FrameworkInfo& frameworkInfo)
{
  ObjectApprover::Object object;
  object.executor_info = &executorInfo;
  object.framework_info = &frameworkInfo;

  Try<bool> approved = executorsApprover->approved(object);
  if (approved.isError()) {
    LOG(WARNING) << "Error during ExecutorInfo authorization: "
             << approved.error();
    // TODO(joerg84): Consider exposing these errors to the caller.
    return false;
  }
  return approved.get();
}


// Forward declaration for `FullFrameworkWriter`.
static void json(JSON::ObjectWriter* writer, const Summary<Framework>& summary);


// Filtered representation of Full<Framework>.
// Executors and Tasks are filtered based on whether the
// user is authorized to view them.
struct FullFrameworkWriter {
  FullFrameworkWriter(
      const Owned<ObjectApprover>& taskApprover,
      const Owned<ObjectApprover>& executorApprover,
      const Framework* framework)
    : taskApprover_(taskApprover),
      executorApprover_(executorApprover),
      framework_(framework) {}

  void operator()(JSON::ObjectWriter* writer) const
  {
    json(writer, Summary<Framework>(*framework_));

    // Add additional fields to those generated by the
    // `Summary<Framework>` overload.
    writer->field("user", framework_->info.user());
    writer->field("failover_timeout", framework_->info.failover_timeout());
    writer->field("checkpoint", framework_->info.checkpoint());
    writer->field("role", framework_->info.role());
    writer->field("registered_time", framework_->registeredTime.secs());
    writer->field("unregistered_time", framework_->unregisteredTime.secs());

    if (framework_->info.has_principal()) {
      writer->field("principal", framework_->info.principal());
    }

    // TODO(bmahler): Consider deprecating this in favor of the split
    // used and offered resources added in `Summary<Framework>`.
    writer->field(
        "resources",
        framework_->totalUsedResources + framework_->totalOfferedResources);

    // TODO(benh): Consider making reregisteredTime an Option.
    if (framework_->registeredTime != framework_->reregisteredTime) {
      writer->field("reregistered_time", framework_->reregisteredTime.secs());
    }

    // Model all of the tasks associated with a framework.
    writer->field("tasks", [this](JSON::ArrayWriter* writer) {
      foreachvalue (const TaskInfo& taskInfo, framework_->pendingTasks) {
        // Skip unauthorized tasks.
        if (!approveViewTaskInfo(taskApprover_, taskInfo, framework_->info)) {
          continue;
        }

        writer->element([this, &taskInfo](JSON::ObjectWriter* writer) {
          writer->field("id", taskInfo.task_id().value());
          writer->field("name", taskInfo.name());
          writer->field("framework_id", framework_->id().value());

          writer->field(
            "executor_id",
            taskInfo.executor().executor_id().value());

          writer->field("slave_id", taskInfo.slave_id().value());
          writer->field("state", TaskState_Name(TASK_STAGING));
          writer->field("resources", Resources(taskInfo.resources()));
          writer->field("statuses", std::initializer_list<TaskStatus>{});

          if (taskInfo.has_labels()) {
            writer->field("labels", taskInfo.labels());
          }

          if (taskInfo.has_discovery()) {
            writer->field("discovery", JSON::Protobuf(taskInfo.discovery()));
          }

          if (taskInfo.has_container()) {
            writer->field("container", JSON::Protobuf(taskInfo.container()));
          }
        });
      }

      foreachvalue (Task* task, framework_->tasks) {
        // Skip unauthorized tasks.
        if (!approveViewTask(taskApprover_, *task, framework_->info)) {
          continue;
        }

        writer->element(*task);
      }
    });

    writer->field("completed_tasks", [this](JSON::ArrayWriter* writer) {
      foreach (const std::shared_ptr<Task>& task, framework_->completedTasks) {
        // Skip unauthorized tasks.
        if (!approveViewTask(taskApprover_, *task.get(), framework_->info)) {
          continue;
        }

        writer->element(*task);
      }
    });

    // Model all of the offers associated with a framework.
    writer->field("offers", [this](JSON::ArrayWriter* writer) {
      foreach (Offer* offer, framework_->offers) {
        writer->element(*offer);
      }
    });

    // Model all of the executors of a framework.
    writer->field("executors", [this](JSON::ArrayWriter* writer) {
      foreachpair (
          const SlaveID& slaveId,
          const auto& executorsMap,
          framework_->executors) {
        foreachvalue (const ExecutorInfo& executor, executorsMap) {
          writer->element([this,
                           &executor,
                           &slaveId](JSON::ObjectWriter* writer) {
            // Skip unauthorized executors.
            if (!approveViewExecutorInfo(
                    executorApprover_,
                    executor,
                    framework_->info)) {
              return;
            }

            json(writer, executor);
            writer->field("slave_id", slaveId.value());
          });
        }
      }
    });

    // Model all of the labels associated with a framework.
    if (framework_->info.has_labels()) {
      writer->field("labels", framework_->info.labels());
    }
  }

  const Owned<ObjectApprover>& taskApprover_;
  const Owned<ObjectApprover>& executorApprover_;
  const Framework* framework_;
};


static void json(JSON::ObjectWriter* writer, const Summary<Slave>& summary)
{
  const Slave& slave = summary;

  writer->field("id", slave.id.value());
  writer->field("pid", string(slave.pid));
  writer->field("hostname", slave.info.hostname());
  writer->field("registered_time", slave.registeredTime.secs());

  if (slave.reregisteredTime.isSome()) {
    writer->field("reregistered_time", slave.reregisteredTime.get().secs());
  }

  const Resources& totalResources = slave.totalResources;
  writer->field("resources", totalResources);
  writer->field("used_resources", Resources::sum(slave.usedResources));
  writer->field("offered_resources", slave.offeredResources);
  writer->field("reserved_resources", totalResources.reservations());
  writer->field("unreserved_resources", totalResources.unreserved());

  writer->field("attributes", Attributes(slave.info.attributes()));
  writer->field("active", slave.active);
  writer->field("version", slave.version);
}


static void json(JSON::ObjectWriter* writer, const Full<Slave>& full)
{
  const Slave& slave = full;

  json(writer, Summary<Slave>(slave));
}


static void json(JSON::ObjectWriter* writer, const Summary<Framework>& summary)
{
  const Framework& framework = summary;

  writer->field("id", framework.id().value());
  writer->field("name", framework.info.name());

  // Omit pid for http frameworks.
  if (framework.pid.isSome()) {
    writer->field("pid", string(framework.pid.get()));
  }

  // TODO(bmahler): Use these in the webui.
  writer->field("used_resources", framework.totalUsedResources);
  writer->field("offered_resources", framework.totalOfferedResources);
  writer->field("capabilities", framework.info.capabilities());
  writer->field("hostname", framework.info.hostname());
  writer->field("webui_url", framework.info.webui_url());
  writer->field("active", framework.active);
}


static void json(JSON::ObjectWriter* writer, const Full<Framework>& full)
{
  // Use the `FullFrameworkWriter` with `AcceptingObjectApprover`.

  Future<Owned<ObjectApprover>> tasksApprover =
    Owned<ObjectApprover>(new AcceptingObjectApprover());
  Future<Owned<ObjectApprover>> executorsApprover =
    Owned<ObjectApprover>(new AcceptingObjectApprover());

  const Framework& framework = full;

  // Note that we know the futures are ready.
  auto frameworkWriter = FullFrameworkWriter(
      tasksApprover.get(),
      executorsApprover.get(),
      &framework);

  frameworkWriter(writer);
}


void Master::Http::log(const Request& request)
{
  Option<string> userAgent = request.headers.get("User-Agent");
  Option<string> forwardedFor = request.headers.get("X-Forwarded-For");

  LOG(INFO) << "HTTP " << request.method << " for " << request.url.path
            << " from " << request.client
            << (userAgent.isSome()
                ? " with User-Agent='" + userAgent.get() + "'"
                : "")
            << (forwardedFor.isSome()
                ? " with X-Forwarded-For='" + forwardedFor.get() + "'"
                : "");
}


string Master::Http::API_HELP()
{
  return HELP(
    TLDR(
        "Endpoint for API calls against the master."),
    DESCRIPTION(
        "Returns 200 OK when the request was processed sucessfully.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found."),
    AUTHENTICATION(true));
}


Future<Response> Master::Http::api(
    const Request& request,
    const Option<string>& principal) const
{
  // TODO(vinod): Add metrics for rejected requests.

  // TODO(vinod): Add support for rate limiting.

  // When current master is not the leader, redirect to the leading master.
  // Note that this could happen when an operator, or some other
  // service, including a scheduler realizes this is the leading
  // master before the master itself realizes it, e.g., due to a
  // ZooKeeper watch delay.
  if (!master->elected()) {
    return redirect(request);
  }

  CHECK_SOME(master->recovered);

  if (!master->recovered.get().isReady()) {
    return ServiceUnavailable("Master has not finished recovery");
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  v1::master::Call call;

  // TODO(anand): Content type values are case-insensitive.
  Option<string> contentType = request.headers.get("Content-Type");

  if (contentType.isNone()) {
    return BadRequest("Expecting 'Content-Type' to be present");
  }

  if (contentType.get() == APPLICATION_PROTOBUF) {
    if (!call.ParseFromString(request.body)) {
      return BadRequest("Failed to parse body into Call protobuf");
    }
  } else if (contentType.get() == APPLICATION_JSON) {
    Try<JSON::Value> value = JSON::parse(request.body);

    if (value.isError()) {
      return BadRequest("Failed to parse body into JSON: " + value.error());
    }

    Try<v1::master::Call> parse =
      ::protobuf::parse<v1::master::Call>(value.get());

    if (parse.isError()) {
      return BadRequest("Failed to convert JSON into Call protobuf: " +
                        parse.error());
    }

    call = parse.get();
  } else {
    return UnsupportedMediaType(
        string("Expecting 'Content-Type' of ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
  }

  Option<Error> error = validation::master::call::validate(call, principal);

  if (error.isSome()) {
    return BadRequest("Failed to validate v1::master::Call: " +
                      error.get().message);
  }

  LOG(INFO) << "Processing call " << call.type();

  ContentType responseContentType;
  if (request.acceptsMediaType(APPLICATION_JSON)) {
    responseContentType = ContentType::JSON;
  } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
    responseContentType = ContentType::PROTOBUF;
  } else {
    return NotAcceptable(
        string("Expecting 'Accept' to allow ") +
        "'" + APPLICATION_PROTOBUF + "' or '" + APPLICATION_JSON + "'");
  }

  switch (call.type()) {
    case v1::master::Call::UNKNOWN:
      return NotImplemented();

    case v1::master::Call::GET_HEALTH:
      return getHealth(call, principal, responseContentType);

    case v1::master::Call::GET_FLAGS:
      return getFlags(call, principal, responseContentType);

    case v1::master::Call::GET_VERSION:
      return getVersion(call, principal, responseContentType);

    case v1::master::Call::GET_METRICS:
      return NotImplemented();

    case v1::master::Call::GET_LOGGING_LEVEL:
      return getLoggingLevel(call, principal, responseContentType);

    case v1::master::Call::SET_LOGGING_LEVEL:
      return NotImplemented();

    case v1::master::Call::LIST_FILES:
      return NotImplemented();

    case v1::master::Call::READ_FILE:
      return NotImplemented();

    case v1::master::Call::GET_STATE:
      return NotImplemented();

    case v1::master::Call::GET_STATE_SUMMARY:
      return NotImplemented();

    case v1::master::Call::GET_AGENTS:
      return NotImplemented();

    case v1::master::Call::GET_FRAMEWORKS:
      return NotImplemented();

    case v1::master::Call::GET_TASKS:
      return NotImplemented();

    case v1::master::Call::GET_ROLES:
      return NotImplemented();

    case v1::master::Call::GET_WEIGHTS:
      return NotImplemented();

    case v1::master::Call::UPDATE_WEIGHTS:
      return NotImplemented();

    case v1::master::Call::GET_LEADING_MASTER:
      return getLeadingMaster(call, principal, responseContentType);

    case v1::master::Call::RESERVE_RESOURCES:
      return NotImplemented();

    case v1::master::Call::UNRESERVE_RESOURCES:
      return NotImplemented();

    case v1::master::Call::CREATE_VOLUMES:
      return NotImplemented();

    case v1::master::Call::DESTROY_VOLUMES:
      return NotImplemented();

    case v1::master::Call::GET_MAINTENANCE_STATUS:
      return NotImplemented();

    case v1::master::Call::GET_MAINTENANCE_SCHEDULE:
      return NotImplemented();

    case v1::master::Call::UPDATE_MAINTENANCE_SCHEDULE:
      return NotImplemented();

    case v1::master::Call::START_MAINTENANCE:
      return NotImplemented();

    case v1::master::Call::STOP_MAINTENANCE:
      return NotImplemented();

    case v1::master::Call::GET_QUOTA:
      return NotImplemented();

    case v1::master::Call::SET_QUOTA:
      return NotImplemented();

    case v1::master::Call::REMOVE_QUOTA:
      return NotImplemented();

    case v1::master::Call::SUBSCRIBE:
      return NotImplemented();
  }

  UNREACHABLE();
}


// TODO(ijimenez): Add some information or pointers to help
// users understand the HTTP Event/Call API.
string Master::Http::SCHEDULER_HELP()
{
  return HELP(
    TLDR(
        "Endpoint for schedulers to make calls against the master."),
    DESCRIPTION(
        "Returns 202 Accepted iff the request is accepted.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found."),
    AUTHENTICATION(true));
}


Future<Response> Master::Http::scheduler(
    const Request& request,
    const Option<string>& principal) const
{
  // TODO(vinod): Add metrics for rejected requests.

  // TODO(vinod): Add support for rate limiting.

  // When current master is not the leader, redirect to the leading master.
  // Note that this could happen if the scheduler realizes this is the
  // leading master before the master itself realizes it, e.g., due to
  // a ZooKeeper watch delay.
  if (!master->elected()) {
    return redirect(request);
  }

  CHECK_SOME(master->recovered);

  if (!master->recovered.get().isReady()) {
    return ServiceUnavailable("Master has not finished recovery");
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  v1::scheduler::Call v1Call;

  // TODO(anand): Content type values are case-insensitive.
  Option<string> contentType = request.headers.get("Content-Type");

  if (contentType.isNone()) {
    return BadRequest("Expecting 'Content-Type' to be present");
  }

  if (contentType.get() == APPLICATION_PROTOBUF) {
    if (!v1Call.ParseFromString(request.body)) {
      return BadRequest("Failed to parse body into Call protobuf");
    }
  } else if (contentType.get() == APPLICATION_JSON) {
    Try<JSON::Value> value = JSON::parse(request.body);

    if (value.isError()) {
      return BadRequest("Failed to parse body into JSON: " + value.error());
    }

    Try<v1::scheduler::Call> parse =
      ::protobuf::parse<v1::scheduler::Call>(value.get());

    if (parse.isError()) {
      return BadRequest("Failed to convert JSON into Call protobuf: " +
                        parse.error());
    }

    v1Call = parse.get();
  } else {
    return UnsupportedMediaType(
        string("Expecting 'Content-Type' of ") +
        APPLICATION_JSON + " or " + APPLICATION_PROTOBUF);
  }

  scheduler::Call call = devolve(v1Call);

  Option<Error> error = validation::scheduler::call::validate(call, principal);

  if (error.isSome()) {
    return BadRequest("Failed to validate scheduler::Call: " +
                      error.get().message);
  }

  if (call.type() == scheduler::Call::SUBSCRIBE) {
    // We default to JSON 'Content-Type' in the response since an
    // empty 'Accept' header results in all media types considered
    // acceptable.
    ContentType responseContentType = ContentType::JSON;

    if (request.acceptsMediaType(APPLICATION_JSON)) {
      responseContentType = ContentType::JSON;
    } else if (request.acceptsMediaType(APPLICATION_PROTOBUF)) {
      responseContentType = ContentType::PROTOBUF;
    } else {
      return NotAcceptable(
          string("Expecting 'Accept' to allow ") +
          "'" + APPLICATION_PROTOBUF + "' or '" + APPLICATION_JSON + "'");
    }

    // Make sure that a stream ID was not included in the request headers.
    if (request.headers.contains("Mesos-Stream-Id")) {
      return BadRequest(
          "Subscribe calls should not include the 'Mesos-Stream-Id' header");
    }

    const FrameworkInfo& frameworkInfo = call.subscribe().framework_info();

    if (principal.isSome() && !frameworkInfo.has_principal()) {
      // We allow an authenticated framework to not specify a principal
      // in `FrameworkInfo` but we'd prefer to log a WARNING here. We also
      // set `FrameworkInfo.principal` to the value of authenticated principal
      // and use it for authorization later when it happens.
      if (!frameworkInfo.has_principal()) {
        LOG(WARNING)
          << "Setting 'principal' in FrameworkInfo to '" << principal.get()
          << "' because the framework authenticated with that principal but "
          << "did not set it in FrameworkInfo";

        call.mutable_subscribe()->mutable_framework_info()->set_principal(
            principal.get());
      }
    }

    Pipe pipe;
    OK ok;
    ok.headers["Content-Type"] = stringify(responseContentType);

    ok.type = Response::PIPE;
    ok.reader = pipe.reader();

    // Generate a stream ID and return it in the response.
    UUID streamId = UUID::random();
    ok.headers["Mesos-Stream-Id"] = streamId.toString();

    HttpConnection http {pipe.writer(), responseContentType, streamId};
    master->subscribe(http, call.subscribe());

    return ok;
  }

  // We consolidate the framework lookup logic here because it is
  // common for all the call handlers.
  Framework* framework = master->getFramework(call.framework_id());

  if (framework == nullptr) {
    return BadRequest("Framework cannot be found");
  }

  if (principal.isSome() && principal != framework->info.principal()) {
    return BadRequest(
        "Authenticated principal '" + principal.get() + "' does not "
        "match principal '" + framework->info.principal() + "' set in "
        "`FrameworkInfo`");
  }

  if (!framework->connected) {
    return Forbidden("Framework is not subscribed");
  }

  if (framework->http.isNone()) {
    return Forbidden("Framework is not connected via HTTP");
  }

  // This isn't a `SUBSCRIBE` call, so the request should include a stream ID.
  if (!request.headers.contains("Mesos-Stream-Id")) {
    return BadRequest(
        "All non-subscribe calls should include the 'Mesos-Stream-Id' header");
  }

  if (request.headers.at("Mesos-Stream-Id") !=
      framework->http.get().streamId.toString()) {
    return BadRequest(
        "The stream ID included in this request didn't match the stream ID "
        "currently associated with framework ID '"
        + framework->id().value() + "'");
  }

  switch (call.type()) {
    case scheduler::Call::SUBSCRIBE:
      // SUBSCRIBE call should have been handled above.
      LOG(FATAL) << "Unexpected 'SUBSCRIBE' call";

    case scheduler::Call::TEARDOWN:
      master->removeFramework(framework);
      return Accepted();

    case scheduler::Call::ACCEPT:
      master->accept(framework, call.accept());
      return Accepted();

    case scheduler::Call::DECLINE:
      master->decline(framework, call.decline());
      return Accepted();

    case scheduler::Call::ACCEPT_INVERSE_OFFERS:
      master->acceptInverseOffers(framework, call.accept_inverse_offers());
      return Accepted();

    case scheduler::Call::DECLINE_INVERSE_OFFERS:
      master->declineInverseOffers(framework, call.decline_inverse_offers());
      return Accepted();

    case scheduler::Call::REVIVE:
      master->revive(framework);
      return Accepted();

    case scheduler::Call::SUPPRESS:
      master->suppress(framework);
      return Accepted();

    case scheduler::Call::KILL:
      master->kill(framework, call.kill());
      return Accepted();

    case scheduler::Call::SHUTDOWN:
      master->shutdown(framework, call.shutdown());
      return Accepted();

    case scheduler::Call::ACKNOWLEDGE:
      master->acknowledge(framework, call.acknowledge());
      return Accepted();

    case scheduler::Call::RECONCILE:
      master->reconcile(framework, call.reconcile());
      return Accepted();

    case scheduler::Call::MESSAGE:
      master->message(framework, call.message());
      return Accepted();

    case scheduler::Call::REQUEST:
      master->request(framework, call.request());
      return Accepted();

    case scheduler::Call::UNKNOWN:
      LOG(WARNING) << "Received 'UNKNOWN' call";
      return NotImplemented();
  }

  return NotImplemented();
}


string Master::Http::CREATE_VOLUMES_HELP()
{
  return HELP(
    TLDR(
        "Create persistent volumes on reserved resources."),
    DESCRIPTION(
        "Returns 202 ACCEPTED which indicates that the create",
        "operation has been validated successfully by the master.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "The request is then forwarded asynchronously to the Mesos",
        "agent where the reserved resources are located.",
        "That asynchronous message may not be delivered or",
        "creating the volumes at the agent might fail.",
        "",
        "Please provide \"slaveId\" and \"volumes\" values designating",
        "the volumes to be created."),
    AUTHENTICATION(true));
}


static Resources removeDiskInfos(const Resources& resources)
{
  Resources result;

  foreach (Resource resource, resources) {
    resource.clear_disk();
    result += resource;
  }

  return result;
}


Future<Response> Master::Http::createVolumes(
    const Request& request,
    const Option<string>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the query string in the request body.
  Try<hashmap<string, string>> decode =
    process::http::query::decode(request.body);

  if (decode.isError()) {
    return BadRequest("Unable to decode query string: " + decode.error());
  }

  const hashmap<string, string>& values = decode.get();

  Option<string> value;

  value = values.get("slaveId");
  if (value.isNone()) {
    return BadRequest("Missing 'slaveId' query parameter");
  }

  SlaveID slaveId;
  slaveId.set_value(value.get());

  Slave* slave = master->slaves.registered.get(slaveId);
  if (slave == nullptr) {
    return BadRequest("No agent found with specified ID");
  }

  value = values.get("volumes");
  if (value.isNone()) {
    return BadRequest("Missing 'volumes' query parameter");
  }

  Try<JSON::Array> parse =
    JSON::parse<JSON::Array>(value.get());

  if (parse.isError()) {
    return BadRequest(
        "Error in parsing 'volumes' query parameter: " + parse.error());
  }

  Resources volumes;
  foreach (const JSON::Value& value, parse.get().values) {
    Try<Resource> volume = ::protobuf::parse<Resource>(value);
    if (volume.isError()) {
      return BadRequest(
          "Error in parsing 'volumes' query parameter: " + volume.error());
    }
    volumes += volume.get();
  }

  // Create an offer operation.
  Offer::Operation operation;
  operation.set_type(Offer::Operation::CREATE);
  operation.mutable_create()->mutable_volumes()->CopyFrom(volumes);

  Option<Error> validate = validation::operation::validate(
      operation.create(), slave->checkpointedResources, principal);

  if (validate.isSome()) {
    return BadRequest("Invalid CREATE operation: " + validate.get().message);
  }

  return master->authorizeCreateVolume(operation.create(), principal)
    .then(defer(master->self(), [=](bool authorized) -> Future<Response> {
      if (!authorized) {
        return Forbidden();
      }

      // The resources required for this operation are equivalent to the
      // volumes specified by the user minus any DiskInfo (DiskInfo will
      // be created when this operation is applied).
      return _operation(slaveId, removeDiskInfos(volumes), operation);
    }));
}


string Master::Http::DESTROY_VOLUMES_HELP()
{
  return HELP(
    TLDR(
        "Destroy persistent volumes."),
    DESCRIPTION(
        "Returns 202 ACCEPTED which indicates that the destroy",
        "operation has been validated successfully by the master.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "The request is then forwarded asynchronously to the Mesos",
        "agent where the reserved resources are located.",
        "That asynchronous message may not be delivered or",
        "destroying the volumes at the agent might fail.",
        "",
        "Please provide \"slaveId\" and \"volumes\" values designating",
        "the volumes to be destroyed."),
    AUTHENTICATION(true));
}


Future<Response> Master::Http::destroyVolumes(
    const Request& request,
    const Option<string>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the query string in the request body.
  Try<hashmap<string, string>> decode =
    process::http::query::decode(request.body);

  if (decode.isError()) {
    return BadRequest("Unable to decode query string: " + decode.error());
  }

  const hashmap<string, string>& values = decode.get();

  Option<string> value;

  value = values.get("slaveId");
  if (value.isNone()) {
    return BadRequest("Missing 'slaveId' query parameter");
  }

  SlaveID slaveId;
  slaveId.set_value(value.get());

  Slave* slave = master->slaves.registered.get(slaveId);
  if (slave == nullptr) {
    return BadRequest("No agent found with specified ID");
  }

  value = values.get("volumes");
  if (value.isNone()) {
    return BadRequest("Missing 'volumes' query parameter");
  }

  Try<JSON::Array> parse =
    JSON::parse<JSON::Array>(value.get());

  if (parse.isError()) {
    return BadRequest(
        "Error in parsing 'volumes' query parameter: " + parse.error());
  }

  Resources volumes;
  foreach (const JSON::Value& value, parse.get().values) {
    Try<Resource> volume = ::protobuf::parse<Resource>(value);
    if (volume.isError()) {
      return BadRequest(
          "Error in parsing 'volumes' query parameter: " + volume.error());
    }
    volumes += volume.get();
  }

  // Create an offer operation.
  Offer::Operation operation;
  operation.set_type(Offer::Operation::DESTROY);
  operation.mutable_destroy()->mutable_volumes()->CopyFrom(volumes);

  Option<Error> validate = validation::operation::validate(
      operation.destroy(), slave->checkpointedResources);

  if (validate.isSome()) {
    return BadRequest("Invalid DESTROY operation: " + validate.get().message);
  }

  return master->authorizeDestroyVolume(operation.destroy(), principal)
    .then(defer(master->self(), [=](bool authorized) -> Future<Response> {
      if (!authorized) {
        return Forbidden();
      }

      return _operation(slaveId, volumes, operation);
    }));
}


string Master::Http::FRAMEWORKS_HELP()
{
  return HELP(
    TLDR("Exposes the frameworks info."),
    DESCRIPTION(
        "Returns 200 OK when the frameworks info was queried successfully.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found."),
    AUTHENTICATION(true));
}


Future<Response> Master::Http::frameworks(
    const Request& request,
    const Option<string>& /*principal*/) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  auto frameworks = [this](JSON::ObjectWriter* writer) {
    // Model all of the frameworks.
    writer->field("frameworks", [this](JSON::ArrayWriter* writer) {
      foreachvalue (Framework* framework, master->frameworks.registered) {
        writer->element(Full<Framework>(*framework));
      }
    });

    // Model all of the completed frameworks.
    writer->field("completed_frameworks", [this](JSON::ArrayWriter* writer) {
      foreach (const std::shared_ptr<Framework>& framework,
               master->frameworks.completed) {
        writer->element(Full<Framework>(*framework));
      }
    });

    // Model all currently unregistered frameworks. This can happen
    // when a framework has yet to re-register after master failover.
    writer->field("unregistered_frameworks", [this](JSON::ArrayWriter* writer) {
      // Find unregistered frameworks.
      foreachvalue (const Slave* slave, master->slaves.registered) {
        foreachkey (const FrameworkID& frameworkId, slave->tasks) {
          if (!master->frameworks.registered.contains(frameworkId)) {
            writer->element(frameworkId.value());
          }
        }
      }
    });
  };

  return OK(jsonify(frameworks), request.url.query.get("jsonp"));
}


string Master::Http::FLAGS_HELP()
{
  return HELP(
    TLDR("Exposes the master's flag configuration."),
    None(),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Querying this endpoint requires that the current principal",
        "is authorized to query the path.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::flags(
    const Request& request,
    const Option<string>& principal) const
{
  // TODO(nfnt): Remove check for enabled
  // authorization as part of MESOS-5346.
  if (request.method != "GET" && master->authorizer.isSome()) {
    return MethodNotAllowed({"GET"}, request.method);
  }

  Try<string> endpoint = extractEndpoint(request.url);
  if (endpoint.isError()) {
    return Failure("Failed to extract endpoint: " + endpoint.error());
  }

  return authorizeEndpoint(principal, endpoint.get(), request.method)
    .then(defer(
        master->self(),
        [this, request](bool authorized) -> Future<Response> {
          if (!authorized) {
            return Forbidden();
          }

          return OK(_flags(), request.url.query.get("jsonp"));
        }));
}


JSON::Object Master::Http::_flags() const
{
  JSON::Object object;

  {
    JSON::Object flags;
    foreachvalue (const flags::Flag& flag, master->flags) {
      Option<string> value = flag.stringify(master->flags);
      if (value.isSome()) {
        flags.values[flag.effective_name().value] = value.get();
      }
    }
    object.values["flags"] = std::move(flags);
  }

  return object;
}


Future<Response> Master::Http::getFlags(
    const v1::master::Call& call,
    const Option<string>& principal,
    const ContentType& responseContentType) const
{
  CHECK_EQ(v1::master::Call::GET_FLAGS, call.type());

  v1::master::Response response =
    evolve<v1::master::Response::GET_FLAGS>(_flags());

  return OK(serialize(responseContentType, response),
            stringify(responseContentType));
}


string Master::Http::HEALTH_HELP()
{
  return HELP(
    TLDR(
        "Health check of the Master."),
    DESCRIPTION(
        "Returns 200 OK iff the Master is healthy.",
        "Delayed responses are also indicative of poor health."),
    AUTHENTICATION(false));
}


Future<Response> Master::Http::health(const Request& request) const
{
  return OK();
}


Future<Response> Master::Http::getHealth(
    const v1::master::Call& call,
    const Option<string>& principal,
    const ContentType& responseContentType) const
{
  CHECK_EQ(v1::master::Call::GET_HEALTH, call.type());

  v1::master::Response response;
  response.set_type(v1::master::Response::GET_HEALTH);
  response.mutable_get_health()->set_healthy(true);

  return OK(serialize(responseContentType, response),
            stringify(responseContentType));
}


Future<Response> Master::Http::getVersion(
    const v1::master::Call& call,
    const Option<string>& principal,
    const ContentType& responseContentType) const
{
  CHECK_EQ(v1::master::Call::GET_VERSION, call.type());

  v1::master::Response response =
    evolve<v1::master::Response::GET_VERSION>(version());

  return OK(serialize(responseContentType, response),
            stringify(responseContentType));
}


Future<Response> Master::Http::getLoggingLevel(
    const v1::master::Call& call,
    const Option<string>& principal,
    const ContentType& responseContentType) const
{
  CHECK_EQ(v1::master::Call::GET_LOGGING_LEVEL, call.type());

  v1::master::Response response;
  response.set_type(v1::master::Response::GET_LOGGING_LEVEL);
  response.mutable_get_logging_level()->set_level(FLAGS_v);

  return OK(serialize(responseContentType, response),
            stringify(responseContentType));
}


Future<Response> Master::Http::getLeadingMaster(
    const v1::master::Call& call,
    const Option<string>& principal,
    const ContentType& responseContentType) const
{
  CHECK_EQ(v1::master::Call::GET_LEADING_MASTER, call.type());

  v1::master::Response response;
  response.set_type(v1::master::Response::GET_LEADING_MASTER);

  // It is guaranteed that this master has been elected as the leader.
  CHECK(master->elected());

  response.mutable_get_leading_master()->mutable_master_info()->CopyFrom(
    evolve(master->info()));

  return OK(serialize(responseContentType, response),
            stringify(responseContentType));
}


string Master::Http::REDIRECT_HELP()
{
  return HELP(
    TLDR(
        "Redirects to the leading Master."),
    DESCRIPTION(
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "",
        "**NOTES:**",
        "1. This is the recommended way to bookmark the WebUI when",
        "running multiple Masters.",
        "2. This is broken currently \"on the cloud\" (e.g. EC2) as",
        "this will attempt to redirect to the private IP address, unless",
        "advertise_ip points to an externally accessible IP"),
    AUTHENTICATION(false));
}


Future<Response> Master::Http::redirect(const Request& request) const
{
  // If there's no leader, return `ServiceUnavailable`.
  if (master->leader.isNone()) {
    LOG(WARNING) << "Current master is not elected as leader, and leader "
                 << "information is unavailable. Failed to redirect the "
                 << "request url: " << request.url;
    return ServiceUnavailable("No leader elected");
  }

  MasterInfo info = master->leader.get();

  // NOTE: Currently, 'info.ip()' stores ip in network order, which
  // should be fixed. See MESOS-1201 for details.
  Try<string> hostname = info.has_hostname()
    ? info.hostname()
    : net::getHostname(net::IP(ntohl(info.ip())));

  if (hostname.isError()) {
    return InternalServerError(hostname.error());
  }

  LOG(INFO) << "Redirecting request for " << request.url
            << " to the leading master " << hostname.get();

  // NOTE: We can use a protocol-relative URL here in order to allow
  // the browser (or other HTTP client) to prefix with 'http:' or
  // 'https:' depending on the original request. See
  // https://tools.ietf.org/html/rfc7231#section-7.1.2 as well as
  // http://stackoverflow.com/questions/12436669/using-protocol-relative-uris-within-location-headers
  // which discusses this.
  string basePath = "//" + hostname.get() + ":" + stringify(info.port());
  if (request.url.path == "/redirect" ||
      request.url.path == "/" + master->self().id + "/redirect") {
    // When request url is '/redirect' or '/master/redirect', redirect to the
    // base url of leading master to avoid infinite redirect loop.
    return TemporaryRedirect(basePath);
  } else {
    return TemporaryRedirect(basePath + request.url.path);
  }
}


string Master::Http::RESERVE_HELP()
{
  return HELP(
    TLDR(
        "Reserve resources dynamically on a specific agent."),
    DESCRIPTION(
        "Returns 202 ACCEPTED which indicates that the reserve",
        "operation has been validated successfully by the master.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "The request is then forwarded asynchronously to the Mesos",
        "agent where the reserved resources are located.",
        "That asynchronous message may not be delivered or",
        "reserving resources at the agent might fail.",
        "",
        "Please provide \"slaveId\" and \"resources\" values designating",
        "the resources to be reserved."),
    AUTHENTICATION(true));
}


Future<Response> Master::Http::reserve(
    const Request& request,
    const Option<string>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the query string in the request body.
  Try<hashmap<string, string>> decode =
    process::http::query::decode(request.body);

  if (decode.isError()) {
    return BadRequest("Unable to decode query string: " + decode.error());
  }

  const hashmap<string, string>& values = decode.get();

  Option<string> value;

  value = values.get("slaveId");
  if (value.isNone()) {
    return BadRequest("Missing 'slaveId' query parameter");
  }

  SlaveID slaveId;
  slaveId.set_value(value.get());

  Slave* slave = master->slaves.registered.get(slaveId);
  if (slave == nullptr) {
    return BadRequest("No agent found with specified ID");
  }

  value = values.get("resources");
  if (value.isNone()) {
    return BadRequest("Missing 'resources' query parameter");
  }

  Try<JSON::Array> parse =
    JSON::parse<JSON::Array>(value.get());

  if (parse.isError()) {
    return BadRequest(
        "Error in parsing 'resources' query parameter: " + parse.error());
  }

  Resources resources;
  foreach (const JSON::Value& value, parse.get().values) {
    Try<Resource> resource = ::protobuf::parse<Resource>(value);
    if (resource.isError()) {
      return BadRequest(
          "Error in parsing 'resources' query parameter: " + resource.error());
    }
    resources += resource.get();
  }

  // Create an offer operation.
  Offer::Operation operation;
  operation.set_type(Offer::Operation::RESERVE);
  operation.mutable_reserve()->mutable_resources()->CopyFrom(resources);

  Option<Error> error = validation::operation::validate(
      operation.reserve(), principal);

  if (error.isSome()) {
    return BadRequest("Invalid RESERVE operation: " + error.get().message);
  }

  return master->authorizeReserveResources(operation.reserve(), principal)
    .then(defer(master->self(), [=](bool authorized) -> Future<Response> {
      if (!authorized) {
        return Forbidden();
      }

      // NOTE: `flatten()` is important. To make a dynamic reservation,
      // we want to ensure that the required resources are available
      // and unreserved; `flatten()` removes the role and
      // ReservationInfo from the resources.
      return _operation(slaveId, resources.flatten(), operation);
    }));
}


string Master::Http::SLAVES_HELP()
{
  return HELP(
    TLDR(
        "Information about registered agents."),
    DESCRIPTION(
        "Returns 200 OK when the request was processed successfully.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "This endpoint shows information about the agents registered in",
        "this master formatted as a JSON object."),
    AUTHENTICATION(true));
}


Future<Response> Master::Http::slaves(
    const Request& request,
    const Option<string>& /*principal*/) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  auto slaves = [this](JSON::ObjectWriter* writer) {
    writer->field("slaves", [this](JSON::ArrayWriter* writer) {
      foreachvalue (const Slave* slave, master->slaves.registered) {
        writer->element([&slave](JSON::ObjectWriter* writer) {
          json(writer, Full<Slave>(*slave));

          // Add the complete protobuf->JSON for all used, reserved,
          // and offered resources. The other endpoints summarize
          // resource information, which omits the details of
          // reservations and persistent volumes. Full resource
          // information is necessary so that operators can use the
          // `/unreserve` and `/destroy-volumes` endpoints.

          hashmap<string, Resources> reserved =
            slave->totalResources.reservations();

          writer->field(
              "reserved_resources_full",
              [&reserved](JSON::ObjectWriter* writer) {
                foreachpair (const string& role,
                             const Resources& resources,
                             reserved) {
                  writer->field(role, [&resources](JSON::ArrayWriter* writer) {
                    foreach (const Resource& resource, resources) {
                      writer->element(JSON::Protobuf(resource));
                    }
                  });
                }
              });

          Resources usedResources = Resources::sum(slave->usedResources);

          writer->field(
              "used_resources_full",
              [&usedResources](JSON::ArrayWriter* writer) {
                foreach (const Resource& resource, usedResources) {
                  writer->element(JSON::Protobuf(resource));
                }
              });

          const Resources& offeredResources = slave->offeredResources;

          writer->field(
              "offered_resources_full",
              [&offeredResources](JSON::ArrayWriter* writer) {
                foreach (const Resource& resource, offeredResources) {
                  writer->element(JSON::Protobuf(resource));
                }
              });
        });
      };
    });
  };

  return OK(jsonify(slaves), request.url.query.get("jsonp"));
}


string Master::Http::QUOTA_HELP()
{
  return HELP(
    TLDR(
        "Gets or updates quota for roles."),
    DESCRIPTION(
        "Returns 200 OK when the quota was queried or updated successfully.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "GET: Returns the currently set quotas as JSON.",
        "",
        "POST: Validates the request body as JSON",
        " and sets quota for a role.",
        "",
        "DELETE: Validates the request body as JSON",
        " and removes quota for a role."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Using this endpoint to set a quota for a certain role requires that",
        "the current principal is authorized to set quota for the target role.",
        "Similarly, removing quota requires that the principal is authorized",
        "to remove quota created by the quota_principal.",
        "Getting quota information for a certain role requires that the",
        "current principal is authorized to get quota for the target role,",
        "otherwise the entry for the target role could be silently filtered.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::quota(
    const Request& request,
    const Option<string>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  // Dispatch based on HTTP method to separate `QuotaHandler`.
  if (request.method == "GET") {
    return quotaHandler.status(request, principal);
  }

  if (request.method == "POST") {
    return quotaHandler.set(request, principal);
  }

  if (request.method == "DELETE") {
    return quotaHandler.remove(request, principal);
  }

  // TODO(joerg84): Add update logic for PUT requests
  // once Quota supports updates.

  return MethodNotAllowed({"GET", "POST", "DELETE"}, request.method);
}


string Master::Http::WEIGHTS_HELP()
{
  return HELP(
    TLDR(
        "Updates weights for the specified roles."),
    DESCRIPTION(
        "Returns 200 OK when the weights update was successful.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "PUT: Validates the request body as JSON",
        "and updates the weights for the specified roles."),
    AUTHENTICATION(true),
    AUTHORIZATION(
        "Getting weight information for a certain role requires that the",
        "current principal is authorized to get weights for the target role,",
        "otherwise the entry for the target role could be silently filtered.",
        "See the authorization documentation for details."));
}


Future<Response> Master::Http::weights(
    const Request& request,
    const Option<string>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  // TODO(Yongqiao Wang): `/roles` endpoint also shows the weights information,
  // consider erasing the duplicated information later.
  if (request.method == "GET") {
    return weightsHandler.get(request, principal);
  }

  // Dispatch based on HTTP method to separate `WeightsHandler`.
  if (request.method == "PUT") {
    return weightsHandler.update(request, principal);
  }

  return MethodNotAllowed({"GET", "PUT"}, request.method);
}


string Master::Http::STATE_HELP()
{
  return HELP(
    TLDR(
        "Information about state of master."),
    DESCRIPTION(
        "Returns 200 OK when the state of the master was queried successfully.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "This endpoint shows information about the frameworks, tasks,",
        "executors and agents running in the cluster as a JSON object.",
        "",
        "Example (**Note**: this is not exhaustive):",
        "",
        "```",
        "{",
        "    \"version\" : \"0.28.0\",",
        "    \"git_sha\" : \"9d5889b5a265849886a533965f4aefefd1fbd103\",",
        "    \"git_branch\" : \"refs/heads/master\",",
        "    \"git_tag\" : \"0.28.0\",",
        "    \"build_date\" : \"2016-02-15 10:00:28\",",
        "    \"build_time\" : 1455559228,",
        "    \"build_user\" : \"mesos-user\",",
        "    \"start_time\" : 1455643643.42422,",
        "    \"elected_time\" : 1455643643.43457,",
        "    \"id\" : \"b5eac2c5-609b-4ca1-a352-61941702fc9e\",",
        "    \"pid\" : \"master@127.0.0.1:5050\",",
        "    \"hostname\" : \"localhost\",",
        "    \"activated_slaves\" : 0,",
        "    \"deactivated_slaves\" : 0,",
        "    \"cluster\" : \"test-cluster\",",
        "    \"leader\" : \"master@127.0.0.1:5050\",",
        "    \"log_dir\" : \"/var/log\",",
        "    \"external_log_file\" : \"mesos.log\",",
        "    \"flags\" : {",
        "         \"framework_sorter\" : \"drf\",",
        "         \"authenticate\" : \"false\",",
        "         \"logbufsecs\" : \"0\",",
        "         \"initialize_driver_logging\" : \"true\",",
        "         \"work_dir\" : \"/var/lib/mesos\",",
        "         \"http_authenticators\" : \"basic\",",
        "         \"authorizers\" : \"local\",",
        "         \"agent_reregister_timeout\" : \"10mins\",",
        "         \"logging_level\" : \"INFO\",",
        "         \"help\" : \"false\",",
        "         \"root_submissions\" : \"true\",",
        "         \"ip\" : \"127.0.0.1\",",
        "         \"user_sorter\" : \"drf\",",
        "         \"version\" : \"false\",",
        "         \"max_agent_ping_timeouts\" : \"5\",",
        "         \"agent_ping_timeout\" : \"15secs\",",
        "         \"registry_store_timeout\" : \"20secs\",",
        "         \"max_completed_frameworks\" : \"50\",",
        "         \"quiet\" : \"false\",",
        "         \"allocator\" : \"HierarchicalDRF\",",
        "         \"hostname_lookup\" : \"true\",",
        "         \"authenticators\" : \"crammd5\",",
        "         \"max_completed_tasks_per_framework\" : \"1000\",",
        "         \"registry\" : \"replicated_log\",",
        "         \"registry_strict\" : \"false\",",
        "         \"log_auto_initialize\" : \"true\",",
        "         \"authenticate_agents\" : \"false\",",
        "         \"registry_fetch_timeout\" : \"1mins\",",
        "         \"allocation_interval\" : \"1secs\",",
        "         \"authenticate_http\" : \"false\",",
        "         \"port\" : \"5050\",",
        "         \"zk_session_timeout\" : \"10secs\",",
        "         \"recovery_agent_removal_limit\" : \"100%\",",
        "         \"webui_dir\" : \"/path/to/mesos/build/../src/webui\",",
        "         \"cluster\" : \"mycluster\",",
        "         \"leader\" : \"master@127.0.0.1:5050\",",
        "         \"log_dir\" : \"/var/log\",",
        "         \"external_log_file\" : \"mesos.log\"",
        "    },",
        "    \"slaves\" : [],",
        "    \"frameworks\" : [],",
        "    \"completed_frameworks\" : [],",
        "    \"orphan_tasks\" : [],",
        "    \"unregistered_frameworks\" : []",
        "}",
        "```"),
    AUTHENTICATION(true));
}


Future<Response> Master::Http::state(
    const Request& request,
    const Option<string>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  // Retrieve `ObjectApprover`s for authorizing frameworks and tasks.
  Future<Owned<ObjectApprover>> frameworksApprover;
  Future<Owned<ObjectApprover>> tasksApprover;
  Future<Owned<ObjectApprover>> executorsApprover;

  if (master->authorizer.isSome()) {
    authorization::Subject subject;
    if (principal.isSome()) {
      subject.set_value(principal.get());
    }

    frameworksApprover = master->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_FRAMEWORK);

    tasksApprover = master->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_TASK);

    executorsApprover = master->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_EXECUTOR);
  } else {
    frameworksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
    tasksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
    executorsApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return collect(frameworksApprover, tasksApprover, executorsApprover)
    .then(defer(master->self(),
        [=](const tuple<Owned<ObjectApprover>,
                        Owned<ObjectApprover>,
                        Owned<ObjectApprover>>& approvers) -> Response {
      auto state = [this, &approvers](JSON::ObjectWriter* writer) {
        // Get approver from tuple.
        Owned<ObjectApprover> frameworksApprover;
        Owned<ObjectApprover> tasksApprover;
        Owned<ObjectApprover> executorsApprover;
        tie(frameworksApprover, tasksApprover, executorsApprover) = approvers;

        writer->field("version", MESOS_VERSION);

        if (build::GIT_SHA.isSome()) {
          writer->field("git_sha", build::GIT_SHA.get());
        }

        if (build::GIT_BRANCH.isSome()) {
          writer->field("git_branch", build::GIT_BRANCH.get());
        }

        if (build::GIT_TAG.isSome()) {
          writer->field("git_tag", build::GIT_TAG.get());
        }

        writer->field("build_date", build::DATE);
        writer->field("build_time", build::TIME);
        writer->field("build_user", build::USER);
        writer->field("start_time", master->startTime.secs());

        if (master->electedTime.isSome()) {
          writer->field("elected_time", master->electedTime.get().secs());
        }

        writer->field("id", master->info().id());
        writer->field("pid", string(master->self()));
        writer->field("hostname", master->info().hostname());
        writer->field("activated_slaves", master->_slaves_active());
        writer->field("deactivated_slaves", master->_slaves_inactive());

        if (master->flags.cluster.isSome()) {
          writer->field("cluster", master->flags.cluster.get());
        }

        if (master->leader.isSome()) {
          writer->field("leader", master->leader.get().pid());
        }

        if (master->flags.log_dir.isSome()) {
          writer->field("log_dir", master->flags.log_dir.get());
        }

        if (master->flags.external_log_file.isSome()) {
          writer->field("external_log_file",
             master->flags.external_log_file.get());
        }

        writer->field("flags", [this](JSON::ObjectWriter* writer) {
          foreachvalue (const flags::Flag& flag, master->flags) {
            Option<string> value = flag.stringify(master->flags);
            if (value.isSome()) {
              writer->field(flag.effective_name().value, value.get());
            }
          }
        });

        // Model all of the slaves.
        writer->field("slaves", [this](JSON::ArrayWriter* writer) {
          foreachvalue (Slave* slave, master->slaves.registered) {
            writer->element(Full<Slave>(*slave));
          }
        });

        // Model all of the frameworks.
        writer->field(
            "frameworks",
            [this, &frameworksApprover, executorsApprover, &tasksApprover](
                JSON::ArrayWriter* writer) {
              foreachvalue (
                  Framework* framework,
                  master->frameworks.registered) {
                // Skip unauthorized frameworks.
                if (!approveViewFrameworkInfo(
                    frameworksApprover, framework->info)) {
                  continue;
                }

                auto frameworkWriter = FullFrameworkWriter(
                    tasksApprover,
                    executorsApprover,
                    framework);

                writer->element(frameworkWriter);
              }
            });

        // Model all of the completed frameworks.
        writer->field(
            "completed_frameworks",
            [this, &frameworksApprover, executorsApprover, &tasksApprover](
                JSON::ArrayWriter* writer) {
              foreach (
                  const std::shared_ptr<Framework>& framework,
                  master->frameworks.completed) {
                // Skip unauthorized frameworks.
                if (!approveViewFrameworkInfo(
                    frameworksApprover, framework->info)) {
                  continue;
                }

                auto frameworkWriter = FullFrameworkWriter(
                    tasksApprover, executorsApprover, framework.get());

                writer->element(frameworkWriter);
              }
            });

        // Model all of the orphan tasks.
        writer->field("orphan_tasks", [this](JSON::ArrayWriter* writer) {
          // Find those orphan tasks.
          foreachvalue (const Slave* slave, master->slaves.registered) {
            typedef hashmap<TaskID, Task*> TaskMap;
            foreachvalue (const TaskMap& tasks, slave->tasks) {
              foreachvalue (const Task* task, tasks) {
                CHECK_NOTNULL(task);
                if (!master->frameworks.registered.contains(
                    task->framework_id())) {
                  writer->element(*task);
                }
              }
            }
          }
        });

        // Model all currently unregistered frameworks. This can happen
        // when a framework has yet to re-register after master failover.
        writer->field("unregistered_frameworks", [this](
            JSON::ArrayWriter* writer) {
          // Find unregistered frameworks.
          foreachvalue (const Slave* slave, master->slaves.registered) {
            foreachkey (const FrameworkID& frameworkId, slave->tasks) {
              if (!master->frameworks.registered.contains(frameworkId)) {
                writer->element(frameworkId.value());
              }
            }
          }
        });
      };

      return OK(jsonify(state), request.url.query.get("jsonp"));
    }))
    .repair([](const Future<Response>& response) {
      LOG(WARNING) << "Authorization failed: " << response.failure();
      return InternalServerError(response.failure());
    });
}


// This abstraction has no side-effects. It factors out computing the
// mapping from 'slaves' to 'frameworks' to answer the questions 'what
// frameworks are running on a given slave?' and 'what slaves are
// running the given framework?'.
class SlaveFrameworkMapping
{
public:
  SlaveFrameworkMapping(const hashmap<FrameworkID, Framework*>& frameworks)
  {
    foreachpair (const FrameworkID& frameworkId,
                 const Framework* framework,
                 frameworks) {
      foreachvalue (const TaskInfo& taskInfo, framework->pendingTasks) {
        frameworksToSlaves[frameworkId].insert(taskInfo.slave_id());
        slavesToFrameworks[taskInfo.slave_id()].insert(frameworkId);
      }

      foreachvalue (const Task* task, framework->tasks) {
        frameworksToSlaves[frameworkId].insert(task->slave_id());
        slavesToFrameworks[task->slave_id()].insert(frameworkId);
      }

      foreach (const std::shared_ptr<Task>& task, framework->completedTasks) {
        frameworksToSlaves[frameworkId].insert(task->slave_id());
        slavesToFrameworks[task->slave_id()].insert(frameworkId);
      }
    }
  }

  const hashset<FrameworkID>& frameworks(const SlaveID& slaveId) const
  {
    const auto iterator = slavesToFrameworks.find(slaveId);
    return iterator != slavesToFrameworks.end() ?
      iterator->second : hashset<FrameworkID>::EMPTY;
  }

  const hashset<SlaveID>& slaves(const FrameworkID& frameworkId) const
  {
    const auto iterator = frameworksToSlaves.find(frameworkId);
    return iterator != frameworksToSlaves.end() ?
      iterator->second : hashset<SlaveID>::EMPTY;
  }

private:
  hashmap<SlaveID, hashset<FrameworkID>> slavesToFrameworks;
  hashmap<FrameworkID, hashset<SlaveID>> frameworksToSlaves;
};


// This abstraction has no side-effects. It factors out the accounting
// for a 'TaskState' summary. We use this to summarize 'TaskState's
// for both frameworks as well as slaves.
struct TaskStateSummary
{
  // TODO(jmlvanre): Possibly clean this up as per MESOS-2694.
  const static TaskStateSummary EMPTY;

  TaskStateSummary()
    : staging(0),
      starting(0),
      running(0),
      killing(0),
      finished(0),
      killed(0),
      failed(0),
      lost(0),
      error(0) {}

  // Account for the state of the given task.
  void count(const Task& task)
  {
    switch (task.state()) {
      case TASK_STAGING: { ++staging; break; }
      case TASK_STARTING: { ++starting; break; }
      case TASK_RUNNING: { ++running; break; }
      case TASK_KILLING: { ++killing; break; }
      case TASK_FINISHED: { ++finished; break; }
      case TASK_KILLED: { ++killed; break; }
      case TASK_FAILED: { ++failed; break; }
      case TASK_LOST: { ++lost; break; }
      case TASK_ERROR: { ++error; break; }
      // No default case allows for a helpful compiler error if we
      // introduce a new state.
    }
  }

  size_t staging;
  size_t starting;
  size_t running;
  size_t killing;
  size_t finished;
  size_t killed;
  size_t failed;
  size_t lost;
  size_t error;
};


const TaskStateSummary TaskStateSummary::EMPTY;


// This abstraction has no side-effects. It factors out computing the
// 'TaskState' summaries for frameworks and slaves. This answers the
// questions 'How many tasks are in each state for a given framework?'
// and 'How many tasks are in each state for a given slave?'.
class TaskStateSummaries
{
public:
  TaskStateSummaries(const hashmap<FrameworkID, Framework*>& frameworks)
  {
    foreachpair (const FrameworkID& frameworkId,
                 const Framework* framework,
                 frameworks) {
      foreachvalue (const TaskInfo& taskInfo, framework->pendingTasks) {
        frameworkTaskSummaries[frameworkId].staging++;
        slaveTaskSummaries[taskInfo.slave_id()].staging++;
      }

      foreachvalue (const Task* task, framework->tasks) {
        frameworkTaskSummaries[frameworkId].count(*task);
        slaveTaskSummaries[task->slave_id()].count(*task);
      }

      foreach (const std::shared_ptr<Task>& task, framework->completedTasks) {
        frameworkTaskSummaries[frameworkId].count(*task);
        slaveTaskSummaries[task->slave_id()].count(*task);
      }
    }
  }

  const TaskStateSummary& framework(const FrameworkID& frameworkId) const
  {
    const auto iterator = frameworkTaskSummaries.find(frameworkId);
    return iterator != frameworkTaskSummaries.end() ?
      iterator->second : TaskStateSummary::EMPTY;
  }

  const TaskStateSummary& slave(const SlaveID& slaveId) const
  {
    const auto iterator = slaveTaskSummaries.find(slaveId);
    return iterator != slaveTaskSummaries.end() ?
      iterator->second : TaskStateSummary::EMPTY;
  }
private:
  hashmap<FrameworkID, TaskStateSummary> frameworkTaskSummaries;
  hashmap<SlaveID, TaskStateSummary> slaveTaskSummaries;
};


string Master::Http::STATESUMMARY_HELP()
{
  return HELP(
    TLDR(
        "Summary of state of all tasks and registered frameworks in cluster."),
    DESCRIPTION(
        "Returns 200 OK when a summary of the master's state was queried",
        "successfully.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "This endpoint gives a summary of the state of all tasks and",
        "registered frameworks in the cluster as a JSON object."),
    AUTHENTICATION(true));
}


Future<Response> Master::Http::stateSummary(
    const Request& request,
    const Option<string>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  Future<Owned<ObjectApprover>> frameworksApprover;

  if (master->authorizer.isSome()) {
    authorization::Subject subject;
    if (principal.isSome()) {
      subject.set_value(principal.get());
    }

    frameworksApprover = master->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_FRAMEWORK);
  } else {
    frameworksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return frameworksApprover
    .then(defer(master->self(),
        [=](const Owned<ObjectApprover>& frameworksApprover) -> Response {
      auto stateSummary =
          [this, &frameworksApprover](JSON::ObjectWriter* writer) {
        Owned<ObjectApprover> frameworksApprover_ = frameworksApprover;

        writer->field("hostname", master->info().hostname());

        if (master->flags.cluster.isSome()) {
          writer->field("cluster", master->flags.cluster.get());
        }

        // We use the tasks in the 'Frameworks' struct to compute summaries
        // for this endpoint. This is done 1) for consistency between the
        // 'slaves' and 'frameworks' subsections below 2) because we want to
        // provide summary information for frameworks that are currently
        // registered 3) the frameworks keep a circular buffer of completed
        // tasks that we can use to keep a limited view on the history of
        // recent completed / failed tasks.

        // Generate mappings from 'slave' to 'framework' and reverse.
        SlaveFrameworkMapping slaveFrameworkMapping(
            master->frameworks.registered);

        // Generate 'TaskState' summaries for all framework and slave ids.
        TaskStateSummaries taskStateSummaries(master->frameworks.registered);

        // Model all of the slaves.
        writer->field("slaves",
                      [this,
                       &slaveFrameworkMapping,
                       &taskStateSummaries](JSON::ArrayWriter* writer) {
          foreachvalue (Slave* slave, master->slaves.registered) {
            writer->element([&slave,
                             &slaveFrameworkMapping,
                             &taskStateSummaries](JSON::ObjectWriter* writer) {
              json(writer, Summary<Slave>(*slave));

              // Add the 'TaskState' summary for this slave.
              const TaskStateSummary& summary =
                taskStateSummaries.slave(slave->id);

              writer->field("TASK_STAGING", summary.staging);
              writer->field("TASK_STARTING", summary.starting);
              writer->field("TASK_RUNNING", summary.running);
              writer->field("TASK_KILLING", summary.killing);
              writer->field("TASK_FINISHED", summary.finished);
              writer->field("TASK_KILLED", summary.killed);
              writer->field("TASK_FAILED", summary.failed);
              writer->field("TASK_LOST", summary.lost);
              writer->field("TASK_ERROR", summary.error);

              // Add the ids of all the frameworks running on this slave.
              const hashset<FrameworkID>& frameworks =
                slaveFrameworkMapping.frameworks(slave->id);

              writer->field("framework_ids",
                            [&frameworks](JSON::ArrayWriter* writer) {
                foreach (const FrameworkID& frameworkId, frameworks) {
                  writer->element(frameworkId.value());
                }
              });
            });
          }
        });

        // Model all of the frameworks.
        writer->field("frameworks",
                      [this,
                       &slaveFrameworkMapping,
                       &taskStateSummaries,
                       &frameworksApprover_](JSON::ArrayWriter* writer) {
          foreachpair (const FrameworkID& frameworkId,
                       Framework* framework,
                       master->frameworks.registered) {
            // Skip unauthorized frameworks.
            if (!approveViewFrameworkInfo(
                frameworksApprover_,
                framework->info)) {
              continue;
            }

            writer->element([&frameworkId,
                             &framework,
                             &slaveFrameworkMapping,
                             &taskStateSummaries](JSON::ObjectWriter* writer) {
              json(writer, Summary<Framework>(*framework));

              // Add the 'TaskState' summary for this framework.
              const TaskStateSummary& summary =
                taskStateSummaries.framework(frameworkId);

              writer->field("TASK_STAGING", summary.staging);
              writer->field("TASK_STARTING", summary.starting);
              writer->field("TASK_RUNNING", summary.running);
              writer->field("TASK_KILLING", summary.killing);
              writer->field("TASK_FINISHED", summary.finished);
              writer->field("TASK_KILLED", summary.killed);
              writer->field("TASK_FAILED", summary.failed);
              writer->field("TASK_LOST", summary.lost);
              writer->field("TASK_ERROR", summary.error);

              // Add the ids of all the slaves running this framework.
              const hashset<SlaveID>& slaves =
                slaveFrameworkMapping.slaves(frameworkId);

              writer->field("slave_ids", [&slaves](JSON::ArrayWriter* writer) {
                foreach (const SlaveID& slaveId, slaves) {
                  writer->element(slaveId.value());
                }
              });
            });
          }
        });
      };

      return OK(jsonify(stateSummary), request.url.query.get("jsonp"));
    }))
    .repair([](const Future<Response>& response) {
      LOG(WARNING) << "Authorization failed: " << response.failure();
      return InternalServerError(response.failure());
    });
}


string Master::Http::ROLES_HELP()
{
  return HELP(
    TLDR(
        "Information about roles."),
    DESCRIPTION(
        "Returns 200 OK when information about roles was queried successfully.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "This endpoint provides information about roles as a JSON object.",
        "It returns information about every role that is on the role",
        "whitelist (if enabled), has one or more registered frameworks,",
        "or has a non-default weight or quota. For each role, it returns",
        "the weight, total allocated resources, and registered frameworks."),
    AUTHENTICATION(true));
}


// Returns a JSON object modeled after a role.
JSON::Object model(
    const string& name,
    Option<double> weight,
    Option<Role*> _role)
{
  JSON::Object object;
  object.values["name"] = name;

  if (weight.isSome()) {
    object.values["weight"] = weight.get();
  } else {
    object.values["weight"] = 1.0; // Default weight.
  }

  if (_role.isNone()) {
    object.values["resources"] = model(Resources());
    object.values["frameworks"] = JSON::Array();
  } else {
    Role* role = _role.get();

    object.values["resources"] = model(role->resources());

    {
      JSON::Array array;

      foreachkey (const FrameworkID& frameworkId, role->frameworks) {
        array.values.push_back(frameworkId.value());
      }

      object.values["frameworks"] = std::move(array);
    }
  }

  return object;
}


Future<Response> Master::Http::roles(
    const Request& request,
    const Option<string>& /*principal*/) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  JSON::Object object;

  // Compute the role names to return results for. When an explicit
  // role whitelist has been configured, we use that list of names.
  // When using implicit roles, the right behavior is a bit more
  // subtle. There are no constraints on possible role names, so we
  // instead list all the "interesting" roles: the default role ("*"),
  // all roles with one or more registered frameworks, and all roles
  // with a non-default weight or quota.
  //
  // NOTE: we use a `std::set` to store the role names to ensure a
  // deterministic output order.
  set<string> roleList;
  if (master->roleWhitelist.isSome()) {
    const hashset<string>& whitelist = master->roleWhitelist.get();
    roleList.insert(whitelist.begin(), whitelist.end());
  } else {
    roleList.insert("*"); // Default role.
    roleList.insert(
        master->activeRoles.keys().begin(),
        master->activeRoles.keys().end());
    roleList.insert(
        master->weights.keys().begin(),
        master->weights.keys().end());
    roleList.insert(
        master->quotas.keys().begin(),
        master->quotas.keys().end());
  }

  {
    JSON::Array array;

    foreach (const string& name, roleList) {
      Option<double> weight = None();
      if (master->weights.contains(name)) {
        weight = master->weights[name];
      }

      Option<Role*> role = None();
      if (master->activeRoles.contains(name)) {
        role = master->activeRoles[name];
      }

      array.values.push_back(model(name, weight, role));
    }

    object.values["roles"] = std::move(array);
  }

  return OK(object, request.url.query.get("jsonp"));
}


string Master::Http::TEARDOWN_HELP()
{
  return HELP(
    TLDR(
        "Tears down a running framework by shutting down all tasks/executors "
        "and removing the framework."),
    DESCRIPTION(
        "Returns 200 OK if the framework was torn down successfully.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "Please provide a \"frameworkId\" value designating the running",
        "framework to tear down."),
    AUTHENTICATION(true));
}


Future<Response> Master::Http::teardown(
    const Request& request,
    const Option<string>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the query string in the request body (since this is a POST)
  // in order to determine the framework ID to shutdown.
  Try<hashmap<string, string>> decode =
    process::http::query::decode(request.body);

  if (decode.isError()) {
    return BadRequest("Unable to decode query string: " + decode.error());
  }

  const hashmap<string, string>& values = decode.get();

  Option<string> value = values.get("frameworkId");
  if (value.isNone()) {
    return BadRequest("Missing 'frameworkId' query parameter");
  }

  FrameworkID id;
  id.set_value(value.get());

  Framework* framework = master->getFramework(id);

  if (framework == nullptr) {
    return BadRequest("No framework found with specified ID");
  }

  // Skip authorization if no ACLs were provided to the master.
  if (master->authorizer.isNone()) {
    return _teardown(id);
  }

  authorization::Request teardown;
  teardown.set_action(authorization::TEARDOWN_FRAMEWORK_WITH_PRINCIPAL);

  if (principal.isSome()) {
    teardown.mutable_subject()->set_value(principal.get());
  }

  if (framework->info.has_principal()) {
    teardown.mutable_object()->set_value(framework->info.principal());
  }

  return master->authorizer.get()->authorized(teardown)
    .then(defer(master->self(), [=](bool authorized) -> Future<Response> {
      if (!authorized) {
        return Forbidden();
      }
      return _teardown(id);
    }));
}


Future<Response> Master::Http::_teardown(const FrameworkID& id) const
{
  Framework* framework = master->getFramework(id);

  if (framework == nullptr) {
    return BadRequest("No framework found with ID " + stringify(id));
  }

  // TODO(ijimenez): Do 'removeFramework' asynchronously.
  master->removeFramework(framework);

  return OK();
}


string Master::Http::TASKS_HELP()
{
  return HELP(
    TLDR(
        "Lists tasks from all active frameworks."),
    DESCRIPTION(
        "Returns 200 OK when task information was queried successfully.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "Lists known tasks.",
        "",
        "Query parameters:",
        "",
        ">        limit=VALUE          Maximum number of tasks returned "
        "(default is " + stringify(TASK_LIMIT) + ").",
        ">        offset=VALUE         Starts task list at offset.",
        ">        order=(asc|desc)     Ascending or descending sort order "
        "(default is descending)."
        ""),
    AUTHENTICATION(true));
}


struct TaskComparator
{
  static bool ascending(const Task* lhs, const Task* rhs)
  {
    size_t lhsSize = lhs->statuses().size();
    size_t rhsSize = rhs->statuses().size();

    if ((lhsSize == 0) && (rhsSize == 0)) {
      return false;
    }

    if (lhsSize == 0) {
      return true;
    }

    if (rhsSize == 0) {
      return false;
    }

    return (lhs->statuses(0).timestamp() < rhs->statuses(0).timestamp());
  }

  static bool descending(const Task* lhs, const Task* rhs)
  {
    size_t lhsSize = lhs->statuses().size();
    size_t rhsSize = rhs->statuses().size();

    if ((lhsSize == 0) && (rhsSize == 0)) {
      return false;
    }

    if (rhsSize == 0) {
      return true;
    }

    if (lhsSize == 0) {
      return false;
    }

    return (lhs->statuses(0).timestamp() > rhs->statuses(0).timestamp());
  }
};


Future<Response> Master::Http::tasks(
    const Request& request,
    const Option<string>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  // Retrieve Approvers for authorizing frameworks and tasks.
  Future<Owned<ObjectApprover>> frameworksApprover;
  Future<Owned<ObjectApprover>> tasksApprover;
  Future<Owned<ObjectApprover>> executorsApprover;
  if (master->authorizer.isSome()) {
    authorization::Subject subject;
    if (principal.isSome()) {
      subject.set_value(principal.get());
    }

    frameworksApprover = master->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_FRAMEWORK);

    tasksApprover = master->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_TASK);

    executorsApprover = master->authorizer.get()->getObjectApprover(
        subject, authorization::VIEW_EXECUTOR);
  } else {
    frameworksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
    tasksApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
    executorsApprover = Owned<ObjectApprover>(new AcceptingObjectApprover());
  }

  return collect(frameworksApprover, tasksApprover, executorsApprover)
    .then(defer(master->self(),
        [=](const tuple<Owned<ObjectApprover>,
                        Owned<ObjectApprover>,
                        Owned<ObjectApprover>>& approvers) -> Response {
      // Get approver from tuple.
      Owned<ObjectApprover> frameworksApprover;
      Owned<ObjectApprover> tasksApprover;
      Owned<ObjectApprover> executorsApprover;
      tie(frameworksApprover, tasksApprover, executorsApprover) = approvers;

      // Get list options (limit and offset).
      Result<int> result = numify<int>(request.url.query.get("limit"));
      size_t limit = result.isSome() ? result.get() : TASK_LIMIT;

      result = numify<int>(request.url.query.get("offset"));
      size_t offset = result.isSome() ? result.get() : 0;

      // TODO(nnielsen): Currently, formatting errors in offset and/or limit
      // will silently be ignored. This could be reported to the user instead.

      // Construct framework list with both active and completed frameworks.
      vector<const Framework*> frameworks;
      foreachvalue (Framework* framework, master->frameworks.registered) {
        // Skip unauthorized frameworks.
        if (!approveViewFrameworkInfo(frameworksApprover, framework->info)) {
          continue;
        }

        frameworks.push_back(framework);
      }
      foreach (const std::shared_ptr<Framework>& framework,
               master->frameworks.completed) {
        // Skip unauthorized frameworks.
        if (!approveViewFrameworkInfo(frameworksApprover, framework->info)) {
          continue;
        }

        frameworks.push_back(framework.get());
      }

      // Construct task list with both running and finished tasks.
      vector<const Task*> tasks;
      foreach (const Framework* framework, frameworks) {
        foreachvalue (Task* task, framework->tasks) {
          CHECK_NOTNULL(task);
          // Skip unauthorized tasks.
          if (!approveViewTask(tasksApprover, *task, framework->info)) {
            continue;
          }

          tasks.push_back(task);
        }
        foreach (const std::shared_ptr<Task>& task, framework->completedTasks) {
          // Skip unauthorized tasks.
          if (!approveViewTask(tasksApprover, *task.get(), framework->info)) {
            continue;
          }

          tasks.push_back(task.get());
        }
      }

      // Sort tasks by task status timestamp. Default order is descending.
      // The earliest timestamp is chosen for comparison when
      // multiple are present.
      Option<string> order = request.url.query.get("order");
      if (order.isSome() && (order.get() == "asc")) {
        sort(tasks.begin(), tasks.end(), TaskComparator::ascending);
      } else {
        sort(tasks.begin(), tasks.end(), TaskComparator::descending);
      }

      auto tasksWriter = [&tasks, limit, offset](JSON::ObjectWriter* writer) {
        writer->field("tasks",
                      [&tasks, limit, offset](JSON::ArrayWriter* writer) {
          size_t end = std::min(offset + limit, tasks.size());
          for (size_t i = offset; i < end; i++) {
            const Task* task = tasks[i];
            writer->element(*task);
          }
        });
      };

      return OK(jsonify(tasksWriter), request.url.query.get("jsonp"));
  }))
  .repair([](const Future<Response>& response) {
    LOG(WARNING) << "Authorization failed: " << response.failure();
    return InternalServerError(response.failure());
  });
}


// /master/maintenance/schedule endpoint help.
string Master::Http::MAINTENANCE_SCHEDULE_HELP()
{
  return HELP(
    TLDR(
        "Returns or updates the cluster's maintenance schedule."),
    DESCRIPTION(
        "Returns 200 OK when the requested maintenance operation was performed",
        "successfully.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "GET: Returns the current maintenance schedule as JSON.",
        "",
        "POST: Validates the request body as JSON",
        "and updates the maintenance schedule."),
    AUTHENTICATION(true));
}


// /master/maintenance/schedule endpoint handler.
Future<Response> Master::Http::maintenanceSchedule(
    const Request& request,
    const Option<string>& /*principal*/) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "GET" && request.method != "POST") {
    return MethodNotAllowed({"GET", "POST"}, request.method);
  }

  // JSON-ify and return the current maintenance schedule.
  if (request.method == "GET") {
    // TODO(josephw): Return more than one schedule.
    const mesos::maintenance::Schedule schedule =
      master->maintenance.schedules.empty() ?
        mesos::maintenance::Schedule() :
        master->maintenance.schedules.front();

    return OK(JSON::protobuf(schedule), request.url.query.get("jsonp"));
  }

  // Parse the POST body as JSON.
  Try<JSON::Object> jsonSchedule = JSON::parse<JSON::Object>(request.body);
  if (jsonSchedule.isError()) {
    return BadRequest(jsonSchedule.error());
  }

  // Convert the schedule to a protobuf.
  Try<mesos::maintenance::Schedule> protoSchedule =
    ::protobuf::parse<mesos::maintenance::Schedule>(jsonSchedule.get());

  if (protoSchedule.isError()) {
    return BadRequest(protoSchedule.error());
  }

  // Validate that the schedule only transitions machines between
  // `UP` and `DRAINING` modes.
  mesos::maintenance::Schedule schedule = protoSchedule.get();
  Try<Nothing> isValid = maintenance::validation::schedule(
      schedule,
      master->machines);

  if (isValid.isError()) {
    return BadRequest(isValid.error());
  }

  return master->registrar->apply(Owned<Operation>(
      new maintenance::UpdateSchedule(schedule)))
    .then(defer(master->self(), [=](bool result) -> Future<Response> {
      // See the top comment in "master/maintenance.hpp" for why this check
      // is here, and is appropriate.
      CHECK(result);

      // Update the master's local state with the new schedule.
      // NOTE: We only add or remove differences between the current schedule
      // and the new schedule.  This is because the `MachineInfo` struct
      // holds more information than a maintenance schedule.
      // For example, the `mode` field is not part of a maintenance schedule.

      // TODO(josephw): allow more than one schedule.

      // Put the machines in the updated schedule into a set.
      // Save the unavailability, to help with updating some machines.
      hashmap<MachineID, Unavailability> unavailabilities;
      foreach (const mesos::maintenance::Window& window, schedule.windows()) {
        foreach (const MachineID& id, window.machine_ids()) {
          unavailabilities[id] = window.unavailability();
        }
      }

      // NOTE: Copies are needed because `updateUnavailability()` in this loop
      // modifies the container.
      foreachkey (const MachineID& id, utils::copy(master->machines)) {
        // Update the `unavailability` for each existing machine, except for
        // machines going from `UP` to `DRAINING` (handled in the next loop).
        // Each machine will only be touched by 1 of the 2 loops here to
        // avoid sending inverse offer twice for a single machine since
        // `updateUnavailability` will trigger an inverse offer.
        // TODO(gyliu513): Merge this logic with `Master::updateUnavailability`,
        // having it in two places results in more conditionals to handle.
        if (unavailabilities.contains(id)) {
          if (master->machines[id].info.mode() == MachineInfo::UP) {
            continue;
          }

          master->updateUnavailability(id, unavailabilities[id]);
          continue;
        }

        // Transition each removed machine back to the `UP` mode and remove the
        // unavailability.
        master->machines[id].info.set_mode(MachineInfo::UP);
        master->updateUnavailability(id, None());
      }

      // Save each new machine, with the unavailability
      // and starting in `DRAINING` mode.
      foreach (const mesos::maintenance::Window& window, schedule.windows()) {
        foreach (const MachineID& id, window.machine_ids()) {
          if (master->machines.contains(id) &&
              master->machines[id].info.mode() != MachineInfo::UP) {
            continue;
          }

          MachineInfo info;
          info.mutable_id()->CopyFrom(id);
          info.set_mode(MachineInfo::DRAINING);

          master->machines[id].info.CopyFrom(info);

          master->updateUnavailability(id, window.unavailability());
        }
      }

      // Replace the old schedule(s) with the new schedule.
      master->maintenance.schedules.clear();
      master->maintenance.schedules.push_back(schedule);

      return OK();
    }));
}


// /master/machine/down endpoint help.
string Master::Http::MACHINE_DOWN_HELP()
{
  return HELP(
    TLDR(
        "Brings a set of machines down."),
    DESCRIPTION(
        "Returns 200 OK when the operation was successful.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "POST: Validates the request body as JSON and transitions",
        "  the list of machines into DOWN mode.  Currently, only",
        "  machines in DRAINING mode are allowed to be brought down."),
    AUTHENTICATION(true));
}


// /master/machine/down endpoint handler.
Future<Response> Master::Http::machineDown(
    const Request& request,
    const Option<string>& /*principal*/) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the POST body as JSON.
  Try<JSON::Array> jsonIds = JSON::parse<JSON::Array>(request.body);
  if (jsonIds.isError()) {
    return BadRequest(jsonIds.error());
  }

  // Convert the machines to a protobuf.
  auto ids = ::protobuf::parse<RepeatedPtrField<MachineID>>(jsonIds.get());
  if (ids.isError()) {
    return BadRequest(ids.error());
  }

  // Validate every machine in the list.
  Try<Nothing> isValid = maintenance::validation::machines(ids.get());
  if (isValid.isError()) {
    return BadRequest(isValid.error());
  }

  // Check that all machines are part of a maintenance schedule.
  // TODO(josephw): Allow a transition from `UP` to `DOWN`.
  foreach (const MachineID& id, ids.get()) {
    if (!master->machines.contains(id)) {
      return BadRequest(
          "Machine '" + stringify(JSON::protobuf(id)) +
            "' is not part of a maintenance schedule");
    }

    if (master->machines[id].info.mode() != MachineInfo::DRAINING) {
      return BadRequest(
          "Machine '" + stringify(JSON::protobuf(id)) +
            "' is not in DRAINING mode and cannot be brought down");
    }
  }

  return master->registrar->apply(Owned<Operation>(
      new maintenance::StartMaintenance(ids.get())))
    .then(defer(master->self(), [=](bool result) -> Future<Response> {
      // See the top comment in "master/maintenance.hpp" for why this check
      // is here, and is appropriate.
      CHECK(result);

      // We currently send a `ShutdownMessage` to each slave. This terminates
      // all the executors for all the frameworks running on that slave.
      // We also manually remove the slave to force sending TASK_LOST updates
      // for all the tasks that were running on the slave and `LostSlaveMessage`
      // messages to the framework. This guards against the slave having dropped
      // the `ShutdownMessage`.
      foreach (const MachineID& machineId, ids.get()) {
        // The machine may not be in machines. This means no slaves are
        // currently registered on that machine so this is a no-op.
        if (master->machines.contains(machineId)) {
          // NOTE: Copies are needed because removeSlave modifies
          // master->machines.
          foreach (
              const SlaveID& slaveId,
              utils::copy(master->machines[machineId].slaves)) {
            Slave* slave = master->slaves.registered.get(slaveId);
            CHECK_NOTNULL(slave);

            // Tell the slave to shut down.
            ShutdownMessage shutdownMessage;
            shutdownMessage.set_message("Operator initiated 'Machine DOWN'");
            master->send(slave->pid, shutdownMessage);

            // Immediately remove the slave to force sending `TASK_LOST` status
            // updates as well as `LostSlaveMessage` messages to the frameworks.
            // See comment above.
            master->removeSlave(slave, "Operator initiated 'Machine DOWN'");
          }
        }
      }

      // Update the master's local state with the downed machines.
      foreach (const MachineID& id, ids.get()) {
        master->machines[id].info.set_mode(MachineInfo::DOWN);
      }

      return OK();
    }));
}


// /master/maintenance/start endpoint help.
string Master::Http::MACHINE_UP_HELP()
{
  return HELP(
    TLDR(
        "Brings a set of machines back up."),
    DESCRIPTION(
        "Returns 200 OK when the operation was successful.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "POST: Validates the request body as JSON and transitions",
        "  the list of machines into UP mode.  This also removes",
        "  the list of machines from the maintenance schedule."),
    AUTHENTICATION(true));
}


// /master/machine/up endpoint handler.
Future<Response> Master::Http::machineUp(
    const Request& request,
    const Option<string>& /*principal*/) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the POST body as JSON.
  Try<JSON::Array> jsonIds = JSON::parse<JSON::Array>(request.body);
  if (jsonIds.isError()) {
    return BadRequest(jsonIds.error());
  }

  // Convert the machines to a protobuf.
  auto ids = ::protobuf::parse<RepeatedPtrField<MachineID>>(jsonIds.get());
  if (ids.isError()) {
    return BadRequest(ids.error());
  }

  // Validate every machine in the list.
  Try<Nothing> isValid = maintenance::validation::machines(ids.get());
  if (isValid.isError()) {
    return BadRequest(isValid.error());
  }

  // Check that all machines are part of a maintenance schedule.
  foreach (const MachineID& id, ids.get()) {
    if (!master->machines.contains(id)) {
      return BadRequest(
          "Machine '" + stringify(JSON::protobuf(id)) +
            "' is not part of a maintenance schedule");
    }

    if (master->machines[id].info.mode() != MachineInfo::DOWN) {
      return BadRequest(
          "Machine '" + stringify(JSON::protobuf(id)) +
            "' is not in DOWN mode and cannot be brought up");
    }
  }

  return master->registrar->apply(Owned<Operation>(
      new maintenance::StopMaintenance(ids.get())))
    .then(defer(master->self(), [=](bool result) -> Future<Response> {
      // See the top comment in "master/maintenance.hpp" for why this check
      // is here, and is appropriate.
      CHECK(result);

      // Update the master's local state with the reactivated machines.
      hashset<MachineID> updated;
      foreach (const MachineID& id, ids.get()) {
        master->machines[id].info.set_mode(MachineInfo::UP);
        master->machines[id].info.clear_unavailability();
        updated.insert(id);
      }

      // Delete the machines from the schedule.
      for (list<mesos::maintenance::Schedule>::iterator schedule =
          master->maintenance.schedules.begin();
          schedule != master->maintenance.schedules.end();) {
        for (int j = schedule->windows().size() - 1; j >= 0; j--) {
          mesos::maintenance::Window* window = schedule->mutable_windows(j);

          // Delete individual machines.
          for (int k = window->machine_ids().size() - 1; k >= 0; k--) {
            if (updated.contains(window->machine_ids(k))) {
              window->mutable_machine_ids()->DeleteSubrange(k, 1);
            }
          }

          // If the resulting window is empty, delete it.
          if (window->machine_ids().size() == 0) {
            schedule->mutable_windows()->DeleteSubrange(j, 1);
          }
        }

        // If the resulting schedule is empty, delete it.
        if (schedule->windows().size() == 0) {
          schedule = master->maintenance.schedules.erase(schedule);
        } else {
          ++schedule;
        }
      }

      return OK();
    }));
}


// /master/maintenance/status endpoint help.
string Master::Http::MAINTENANCE_STATUS_HELP()
{
  return HELP(
    TLDR(
        "Retrieves the maintenance status of the cluster."),
    DESCRIPTION(
        "Returns 200 OK when the maintenance status was queried successfully.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "Returns an object with one list of machines per machine mode.",
        "For draining machines, this list includes the frameworks' responses",
        "to inverse offers.",
        "**NOTE**:",
        "Inverse offer responses are cleared if the master fails over.",
        "However, new inverse offers will be sent once the master recovers."),
    AUTHENTICATION(true));
}


// /master/maintenance/status endpoint handler.
Future<Response> Master::Http::maintenanceStatus(
    const Request& request,
    const Option<string>& /*principal*/) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "GET") {
    return MethodNotAllowed({"GET"}, request.method);
  }

  return master->allocator->getInverseOfferStatuses()
    .then(defer(
        master->self(),
        [=](
            hashmap<
                SlaveID,
                hashmap<FrameworkID, mesos::master::InverseOfferStatus>> result)
          -> Future<Response> {
    // Unwrap the master's machine information into two arrays of machines.
    // The data is coming from the allocator and therefore could be stale.
    // Also, if the master fails over, this data is cleared.
    mesos::maintenance::ClusterStatus status;
    foreachpair (
        const MachineID& id,
        const Machine& machine,
        master->machines) {
      switch (machine.info.mode()) {
        case MachineInfo::DRAINING: {
          mesos::maintenance::ClusterStatus::DrainingMachine* drainingMachine =
            status.add_draining_machines();

          drainingMachine->mutable_id()->CopyFrom(id);

          // Unwrap inverse offer status information from the allocator.
          foreach (const SlaveID& slave, machine.slaves) {
            if (result.contains(slave)) {
              foreachvalue (
                  const mesos::master::InverseOfferStatus& status,
                  result[slave]) {
                drainingMachine->add_statuses()->CopyFrom(status);
              }
            }
          }
          break;
        }

        case MachineInfo::DOWN: {
          status.add_down_machines()->CopyFrom(id);
          break;
        }

        // Currently, `UP` machines are not specifically tracked in the master.
        case MachineInfo::UP: {}
        default: {
          break;
        }
      }
    }

    return OK(JSON::protobuf(status), request.url.query.get("jsonp"));
  }));
}


string Master::Http::UNRESERVE_HELP()
{
  return HELP(
    TLDR(
        "Unreserve resources dynamically on a specific agent."),
    DESCRIPTION(
        "Returns 202 ACCEPTED which indicates that the unreserve",
        "operation has been validated successfully by the master.",
        "Returns 307 TEMPORARY_REDIRECT redirect to the leading master when",
        "current master is not the leader.",
        "Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be",
        "found.",
        "The request is then forwarded asynchronously to the Mesos",
        "agent where the reserved resources are located.",
        "That asynchronous message may not be delivered or",
        "unreserving resources at the agent might fail.",
        "",
        "Please provide \"slaveId\" and \"resources\" values designating",
        "the resources to be unreserved."),
    AUTHENTICATION(true));
}


Future<Response> Master::Http::unreserve(
    const Request& request,
    const Option<string>& principal) const
{
  // When current master is not the leader, redirect to the leading master.
  if (!master->elected()) {
    return redirect(request);
  }

  if (request.method != "POST") {
    return MethodNotAllowed({"POST"}, request.method);
  }

  // Parse the query string in the request body.
  Try<hashmap<string, string>> decode =
    process::http::query::decode(request.body);

  if (decode.isError()) {
    return BadRequest("Unable to decode query string: " + decode.error());
  }

  const hashmap<string, string>& values = decode.get();

  Option<string> value;

  value = values.get("slaveId");
  if (value.isNone()) {
    return BadRequest("Missing 'slaveId' query parameter");
  }

  SlaveID slaveId;
  slaveId.set_value(value.get());

  Slave* slave = master->slaves.registered.get(slaveId);
  if (slave == nullptr) {
    return BadRequest("No agent found with specified ID");
  }

  value = values.get("resources");
  if (value.isNone()) {
    return BadRequest("Missing 'resources' query parameter");
  }

  Try<JSON::Array> parse =
    JSON::parse<JSON::Array>(value.get());

  if (parse.isError()) {
    return BadRequest(
        "Error in parsing 'resources' query parameter: " + parse.error());
  }

  Resources resources;
  foreach (const JSON::Value& value, parse.get().values) {
    Try<Resource> resource = ::protobuf::parse<Resource>(value);
    if (resource.isError()) {
      return BadRequest(
          "Error in parsing 'resources' query parameter: " + resource.error());
    }
    resources += resource.get();
  }

  // Create an offer operation.
  Offer::Operation operation;
  operation.set_type(Offer::Operation::UNRESERVE);
  operation.mutable_unreserve()->mutable_resources()->CopyFrom(resources);

  Option<Error> error = validation::operation::validate(operation.unreserve());

  if (error.isSome()) {
    return BadRequest(
        "Invalid UNRESERVE operation: " + error.get().message);
  }

  return master->authorizeUnreserveResources(operation.unreserve(), principal)
    .then(defer(master->self(), [=](bool authorized) -> Future<Response> {
      if (!authorized) {
        return Forbidden();
      }

      return _operation(slaveId, resources, operation);
    }));
}


Future<Response> Master::Http::_operation(
    const SlaveID& slaveId,
    Resources required,
    const Offer::Operation& operation) const
{
  Slave* slave = master->slaves.registered.get(slaveId);
  if (slave == nullptr) {
    return BadRequest("No agent found with specified ID");
  }

  // The resources recovered by rescinding outstanding offers.
  Resources recovered;

  // We pessimistically assume that what seems like "available"
  // resources in the allocator will be gone. This can happen due to
  // the race between the allocator scheduling an 'allocate' call to
  // itself vs master's request to schedule 'updateAvailable'.
  // We greedily rescind one offer at time until we've rescinded
  // enough offers to cover 'operation'.
  foreach (Offer* offer, utils::copy(slave->offers)) {
    // If rescinding the offer would not contribute to satisfying
    // the required resources, skip it.
    if (required == required - offer->resources()) {
      continue;
    }

    recovered += offer->resources();
    required -= offer->resources();

    // We explicitly pass 'Filters()' which has a default 'refuse_sec'
    // of 5 seconds rather than 'None()' here, so that we can
    // virtually always win the race against 'allocate'.
    master->allocator->recoverResources(
        offer->framework_id(),
        offer->slave_id(),
        offer->resources(),
        Filters());

    master->removeOffer(offer, true); // Rescind!

    // If we've rescinded enough offers to cover 'operation', we're done.
    Try<Resources> updatedRecovered = recovered.apply(operation);
    if (updatedRecovered.isSome()) {
      break;
    }
  }

  // Propagate the 'Future<Nothing>' as 'Future<Response>' where
  // 'Nothing' -> 'Accepted' and Failed -> 'Conflict'.
  return master->apply(slave, operation)
    .then([]() -> Response { return Accepted(); })
    .repair([](const Future<Response>& result) {
       return Conflict(result.failure());
    });
}


Try<string> Master::Http::extractEndpoint(const process::http::URL& url) const
{
  // Paths are of the form "/master/endpoint". We're only interested
  // in the part after "/master" and tokenize the path accordingly.
  //
  // TODO(nfnt): In the long run, absolute paths for
  // endpoins should be supported, see MESOS-5369.
  const vector<string> pathComponents = strings::tokenize(url.path, "/", 2);

  if (pathComponents.size() < 2u ||
      pathComponents[0] != master->self().id) {
    return Error("Unexpected path '" + url.path + "'");
  }

  return "/" + pathComponents[1];
}


Future<bool> Master::Http::authorizeEndpoint(
    const Option<string>& principal,
    const string& endpoint,
    const string& method) const
{
  if (master->authorizer.isNone()) {
    return true;
  }

  authorization::Request request;

  // TODO(nfnt): Add an additional method when POST requests
  // need to be authorized separately from GET requests.
  if (method == "GET") {
    request.set_action(authorization::GET_ENDPOINT_WITH_PATH);
  } else {
    return Failure("Unexpected request method '" + method + "'");
  }

  if (principal.isSome()) {
    request.mutable_subject()->set_value(principal.get());
  }

  request.mutable_object()->set_value(endpoint);

  LOG(INFO) << "Authorizing principal '"
            << (principal.isSome() ? principal.get() : "ANY")
            << "' to " << method
            << " the '" << endpoint << "' endpoint";

  return master->authorizer.get()->authorized(request);
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
