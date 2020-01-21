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

#include <memory>

#include <mesos/mesos.hpp>

#include <process/async.hpp>
#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include "tests/mesos.hpp"

using std::shared_ptr;

using mesos::authorization::VIEW_EXECUTOR;
using mesos::authorization::VIEW_FLAGS;
using mesos::authorization::VIEW_FRAMEWORK;
using mesos::authorization::VIEW_ROLE;
using mesos::authorization::VIEW_TASK;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::master::detector::MasterDetector;

using process::async;
using process::Clock;
using process::delay;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;
using process::Promise;
using process::Time;

using process::http::Response;
using process::http::Request;
using process::http::Headers;

using testing::SaveArg;


// The tests in this file are designed to verify that the caching
// of read-only requests inside a Mesos master is implemented correctly,
// i.e. cacheable requests are cached and non-cacheable requests will
// return different responses.

namespace mesos {
namespace internal {
namespace tests {

class BlockingAuthorizer;


class MasterLoadTest : public MesosTest {
protected:
  // Describes a given HTTP request.
  struct RequestDescriptor {
    std::string endpoint;
    std::string principal;
    std::string query;
    process::http::Headers headers;

    bool operator<(const RequestDescriptor& other) const;
  };

  // Prepare a mock cluster with 1 master, 1 agent and 1 framework,
  // with the given authorizer being wrapped in a `BlockingAuthorizer`.
  void prepareCluster(Authorizer* authorizer);

  // This function launches a fixed number of equivalent requests per passed
  // request descriptor, while manipulating the master in order to
  // ensure all requests will appear consecutively in the master queue.
  // The returned map associates each response with the descriptor it was
  // created from.
  std::multimap<RequestDescriptor, Future<Response>>
  launchSimultaneousRequests(
      const std::vector<RequestDescriptor>& descriptors);

  // The "mock cluster" created by `prepareCluster()`. These are `protected`
  // so that the test body can access them if required.
  Owned<BlockingAuthorizer> authorizer_;
  Owned<cluster::Master> master_;
  Owned<MasterDetector> detector_;
  Owned<MockScheduler> scheduler_;
  Owned<TestingMesosSchedulerDriver> driver_;
  Owned<cluster::Slave> slave_;
  FrameworkID frameworkId_;
};


// This authorizer will not satisfy any futures from `getApprover()`
// until it is told to, presumably from the test body.
//
// It effectively acts as a giant gate for certain requests.
//
// TODO(asekretenko): Find a way to gate requests without relying on
// authorizer.
class BlockingAuthorizerProcess
  : public process::Process<BlockingAuthorizerProcess>
{
public:
  BlockingAuthorizerProcess(Authorizer* underlying)
    : ProcessBase(process::ID::generate("blocking-authorizer")),
      underlying_(underlying),
      blocked_(false) {}

  Future<bool> authorized(const authorization::Request& request)
  {
    return underlying_->authorized(request);
  }

  Future<shared_ptr<const ObjectApprover>> getApprover(
      const Option<authorization::Subject>& subject,
      const authorization::Action& action)
  {
    Future<shared_ptr<const ObjectApprover>> future =
      underlying_->getApprover(subject, action);

    if (!blocked_) {
      return future;
    }

    // The future is linked to the returned promise in `unleash()`.
    futures_.push(future);
    promises_.emplace();
    return promises_.back().future();
  }

  Future<size_t> pending()
  {
    return promises_.size();
  }

  Future<Nothing> block()
  {
    blocked_ = true;

    return Nothing();
  }

  // Satisfies all future and prending calls made to `getApprover`.
  Future<Nothing> unleash()
  {
    CHECK_EQ(promises_.size(), futures_.size());

    while (!promises_.empty()) {
      promises_.front().associate(futures_.front());

      futures_.pop();
      promises_.pop();
    }

    blocked_ = false;

    return Nothing();
  }

private:
  Authorizer* underlying_;
  std::queue<Future<shared_ptr<const ObjectApprover>>> futures_;
  std::queue<Promise<shared_ptr<const ObjectApprover>>> promises_;
  bool blocked_;
};


class BlockingAuthorizer : public Authorizer
{
public:
  BlockingAuthorizer(Authorizer* underlying)
    : process_(new BlockingAuthorizerProcess(underlying))
  {
    process::spawn(process_.get());
  }

  ~BlockingAuthorizer() override
  {
    process::terminate(process_.get());
    process::wait(process_.get());
  }

  Future<bool> authorized(const authorization::Request& request) override
  {
    return process::dispatch(
        process_.get(),
        &BlockingAuthorizerProcess::authorized,
        request);
  }

  Future<shared_ptr<const ObjectApprover>> getApprover(
      const Option<authorization::Subject>& subject,
      const authorization::Action& action) override
  {
    return process::dispatch(
        process_.get(),
        &BlockingAuthorizerProcess::getApprover,
        subject,
        action);
  }

  Future<size_t> pending()
  {
    return process::dispatch(
        process_.get(),
        &BlockingAuthorizerProcess::pending);
  }

  Future<Nothing> block()
  {
    return process::dispatch(
        process_.get(),
        &BlockingAuthorizerProcess::block);
  }

  Future<Nothing> unleash()
  {
    return process::dispatch(
        process_.get(),
        &BlockingAuthorizerProcess::unleash);
  }

private:
  Owned<BlockingAuthorizerProcess> process_;
};


void MasterLoadTest::prepareCluster(Authorizer* authorizer)
{
  // Start a master.
  authorizer_.reset(new BlockingAuthorizer(authorizer));
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(
      authorizer_.get(), masterFlags);

  ASSERT_SOME(master);
  master_ = master.get();
  detector_ = master_->createDetector();

  Future<FrameworkRegisteredMessage> frameworkRegisteredMessage =
    FUTURE_PROTOBUF(FrameworkRegisteredMessage(), _, _);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start a framework.
  scheduler_.reset(new MockScheduler());
  driver_.reset(new TestingMesosSchedulerDriver(
      scheduler_.get(), detector_.get()));

  EXPECT_CALL(*scheduler_, registered(driver_.get(), _, _))
    .WillOnce(SaveArg<1>(&frameworkId_));

  driver_->start();
  AWAIT_READY(frameworkRegisteredMessage);

  // Start an agent.
  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector_.get(), slaveFlags);

  ASSERT_SOME(slave);
  slave_ = slave.get();

  AWAIT_READY(slaveRegisteredMessage);

  // NOTE: Authorizer is blocked after preparing the cluster, otherwise
  // framework registration, which also uses `prepareObjectApprover(...) will
  // be blocked too.
  authorizer_->block();
}


std::multimap<MasterLoadTest::RequestDescriptor, Future<Response>>
MasterLoadTest::launchSimultaneousRequests(
    const std::vector<RequestDescriptor>& descriptors)
{
  // NOTE: On Mac, the default number of open files (and thus tcp connections)
  // is limited to 256 by default, so this number is tweaked to stay slightly
  // lower than that at 40*5==200 connections for the most demanding test.
  const size_t REQUESTS_PER_DESCRIPTOR = 40;
  const size_t totalRequests = REQUESTS_PER_DESCRIPTOR * descriptors.size();

  std::multimap<RequestDescriptor, Future<Response>> requests;

  // Need this wrapper since `AWAIT_READY()` expects a `void` return type.
  [&] {
    // Send out all http requests based on the specifications
    // found in `descriptors` and store the result in `requests`.
    foreach (const RequestDescriptor& descriptor, descriptors) {
      for (size_t i=0; i < REQUESTS_PER_DESCRIPTOR; ++i) {
        Future<Response> response = process::http::get(
            master_->pid,
            descriptor.endpoint,
            descriptor.query,
            descriptor.headers);

        requests.emplace(descriptor, response);
      }
    }

    // Wait until all the HTTP events have reached the master and are now
    // awaiting authorization.  There might be some other requests that get
    // mixed into the authorizer, so we must have ample requests in the
    // test body to ensure cache hits.
    Time whileLoopStartTime = Clock::now();
    Future<size_t> pendingHttpCalls;
    do {
      pendingHttpCalls = authorizer_->pending();
      AWAIT_READY(pendingHttpCalls);
      // Protect against a potential infinite loop introduced by future bugs.
      ASSERT_TRUE(Clock::now() - whileLoopStartTime < Seconds(20));
    } while (pendingHttpCalls.get() < totalRequests);


    // Now block the master actor, since we don't want the master to start
    // batching until it is queued up with all the HTTP requests.
    // NOTE: This function might be out of scope when the dispatch is
    // scheduled, so we need to pass `masterBlocker` by value.
    auto masterBlocker = std::make_shared<Promise<Nothing>>();
    process::dispatch(master_->pid, [masterBlocker]() {
      masterBlocker->future().await();
    });

    // Unblock the BlockingAuthorizer.
    // This should trigger all the deferrals onto the master from the
    // Authorizer's thread. When this future completes, the master's queue
    // should be full of batched requests.
    AWAIT_READY(authorizer_->unleash());

    // Unblock the master now, so it can perform the batching.
    masterBlocker->set(Nothing());
  }();

  return requests;
}


bool MasterLoadTest::RequestDescriptor::operator<(
    const RequestDescriptor& other) const
{
  return endpoint < other.endpoint;
}


// Test that simultaneous responses to various different endpoints
// all return the expected result.
TEST_F(MasterLoadTest, SimultaneousBatchedRequests)
{
  MockAuthorizer mockAuthorizer;
  prepareCluster(&mockAuthorizer);

  // Set up the actual test.
  RequestDescriptor descriptor1;
  descriptor1.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  descriptor1.endpoint = "/state";

  RequestDescriptor descriptor2 = descriptor1;
  descriptor2.endpoint = "/state-summary";

  RequestDescriptor descriptor3 = descriptor1;
  descriptor3.endpoint = "/frameworks";

  RequestDescriptor descriptor4 = descriptor1;
  descriptor4.endpoint = "/slaves";

  RequestDescriptor descriptor5 = descriptor1;
  descriptor5.endpoint = "/roles";

  auto responses = launchSimultaneousRequests(
      {descriptor1, descriptor2, descriptor3, descriptor4, descriptor5});

  foreachpair (
      const RequestDescriptor& request,
      Future<Response>& response,
      responses)
  {
    AWAIT_READY(response);

    mesos::internal::master::Master* master = master_->master.get();
    mesos::internal::master::Master::ReadOnlyHandler readOnlyHandler(master);

    // TODO(bevers): Ideally we would not use HTTP at all to generate
    // the reference response, but some master-internal function
    // like `model(Summary<Master>)`.
    Try<hashmap<std::string, std::string>> queryParameters_ =
      process::http::query::decode(request.query);

    ASSERT_SOME(queryParameters_);
    hashmap<std::string, std::string> queryParameters = queryParameters_.get();

    process::http::authentication::Principal principal(request.principal);
    MockAuthorizer authorizer;
    Owned<ObjectApprovers> approvers = ObjectApprovers::create(
        &authorizer,
        principal,
        {VIEW_ROLE, VIEW_FLAGS, VIEW_FRAMEWORK, VIEW_TASK, VIEW_EXECUTOR})
      .get();

    Response reference;
    if (request.endpoint == "/state") {
      reference = readOnlyHandler.state(
          ContentType::JSON, queryParameters, approvers).first;
    } else if (request.endpoint == "/state-summary") {
      reference = readOnlyHandler.stateSummary(
          ContentType::JSON, queryParameters, approvers).first;
    } else if (request.endpoint == "/roles") {
      reference = readOnlyHandler.roles(
          ContentType::JSON, queryParameters, approvers).first;
    } else if (request.endpoint == "/frameworks") {
      reference = readOnlyHandler.frameworks(
          ContentType::JSON, queryParameters, approvers).first;
    } else if (request.endpoint == "/slaves") {
      reference = readOnlyHandler.slaves(
          ContentType::JSON, queryParameters, approvers).first;
    } else {
      UNREACHABLE();
    }

    EXPECT_EQ(reference.body, response->body);
  }

  // Ensure that we actually hit the metrics code path while executing
  // the test.
  JSON::Object metrics = Metrics();
  ASSERT_TRUE(metrics.values["master/http_cache_hits"].is<JSON::Number>());
  ASSERT_GT(
      metrics.values["master/http_cache_hits"].as<JSON::Number>().as<size_t>(),
      0u);
}


// Test that simultaneous requests on a single endpoint for two
// different principals return different results.
TEST_F(MasterLoadTest, Principals)
{
  // Set up a proper authorizer for this test.
  master::Flags flags = CreateMasterFlags();

  {
    // Default principal is allowed to view frameworks.
    mesos::ACL::ViewFramework* acl = flags.acls->add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL.principal());
    acl->mutable_users()->set_type(mesos::ACL::Entity::ANY);
  }

  {
    // Default principal 2 is not allowed to view frameworks.
    mesos::ACL::ViewFramework* acl = flags.acls->add_view_frameworks();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_users()->set_type(mesos::ACL::Entity::NONE);
  }

  Authorizer* localAuthorizer = Authorizer::create(flags.acls.get()).get();
  prepareCluster(localAuthorizer);

  // Set up the requests with correct principals.
  RequestDescriptor descriptor1;
  descriptor1.endpoint = "/frameworks";
  descriptor1.principal = DEFAULT_CREDENTIAL.principal();
  descriptor1.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);

  RequestDescriptor descriptor2 = descriptor1;
  descriptor2.principal = DEFAULT_CREDENTIAL_2.principal();
  descriptor2.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL_2);

  auto responses = launchSimultaneousRequests({descriptor1, descriptor2});

  JSON::Value expected = JSON::parse(
      "{"
        "\"frameworks\": [{"
            "\"id\": \""  + stringify(frameworkId_) + "\""
        "}]"
      "}"
  ).get();

  foreachpair (
      const RequestDescriptor& request,
      Future<Response>& response,
      responses)
  {
    AWAIT_READY(response);

    Try<JSON::Value> jsonResponse = JSON::parse(response->body);
    ASSERT_SOME(jsonResponse);

    if (request.principal == DEFAULT_CREDENTIAL.principal()) {
      EXPECT_TRUE(jsonResponse->contains(expected))
        << "Principal " << request.principal
        << " got HTTP response: " << response->body;
    } else {
      EXPECT_FALSE(jsonResponse->contains(expected))
        << "Principal " << request.principal
        << " got HTTP response: " << response->body;
    }
  }

  // Ensure that we actually hit the metrics code path while executing
  // the test.
  JSON::Object metrics = Metrics();
  ASSERT_TRUE(metrics.values["master/http_cache_hits"].is<JSON::Number>());
  ASSERT_GT(
      metrics.values["master/http_cache_hits"].as<JSON::Number>().as<size_t>(),
      0u);
}


// Test that simultaneous requests on a single endpoint with
// different query parameters produce different results.
TEST_F(MasterLoadTest, QueryParameters)
{
  MockAuthorizer mockAuthorizer;
  prepareCluster(&mockAuthorizer);

  RequestDescriptor descriptor1;
  descriptor1.endpoint = "/frameworks";
  descriptor1.headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
  descriptor1.query = "";

  RequestDescriptor descriptor2 = descriptor1;
  descriptor2.query = "framework_id=nonexisting-framework-id";

  RequestDescriptor descriptor3 = descriptor1;
  descriptor3.query = "jsonp=xxx";

  auto responses = launchSimultaneousRequests(
      {descriptor1, descriptor2, descriptor3});

  JSON::Value expected = JSON::parse(
      "{"
        "\"frameworks\": [{"
            "\"id\": \""  + stringify(frameworkId_) + "\""
        "}]"
      "}"
  ).get();

  foreachpair (
      const RequestDescriptor& request,
      Future<Response>& response,
      responses)
  {
    AWAIT_READY(response);

    if (strings::contains(request.query, "jsonp")) {
      EXPECT_TRUE(strings::contains(response->body, "xxx"))
        << "Got HTTP response: " << response->body;
      continue;
    }

    Try<JSON::Value> jsonResponse = JSON::parse(response->body);
    ASSERT_SOME(jsonResponse);

    if (strings::contains(request.query, "framework_id")) {
      EXPECT_FALSE(jsonResponse->contains(expected))
        << "Got HTTP response: " << response->body;
    } else {
      EXPECT_TRUE(jsonResponse->contains(expected))
        << "Got HTTP response: " << response->body;
    }
  }

  // Ensure that we actually hit the metrics code path while executing
  // the test.
  JSON::Object metrics = Metrics();
  ASSERT_TRUE(metrics.values["master/http_cache_hits"].is<JSON::Number>());
  ASSERT_GT(
      metrics.values["master/http_cache_hits"].as<JSON::Number>().as<size_t>(),
      0u);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
