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

#include <mesos/type_utils.hpp>

#include <process/gtest.hpp>
#include <process/gmock.hpp>

#include <stout/fs.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>

#include "common/http.hpp"

#include "internal/evolve.hpp"

#include "slave/slave.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

namespace http = process::http;

using std::list;
using std::string;
using std::vector;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;

using process::Future;
using process::Owned;
using process::PID;

using testing::Values;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

class AgentResourceProviderConfigApiTest
  : public MesosTest,
    public WithParamInterface<ContentType>
{
public:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    resourceProviderConfigDir =
      path::join(sandbox.get(), "resource_provider_configs");

    ASSERT_SOME(os::mkdir(resourceProviderConfigDir));
  }

  ResourceProviderInfo createResourceProviderInfo(const Bytes& capacity)
  {
    const string testCsiPluginWorkDir = path::join(sandbox.get(), "test");
    CHECK_SOME(os::mkdir(testCsiPluginWorkDir));

    string testCsiPluginPath =
      path::join(tests::flags.build_dir, "src", "test-csi-plugin");

    Try<string> resourceProviderConfig = strings::format(
        R"~(
        {
          "type": "org.apache.mesos.rp.local.storage",
          "name": "test",
          "default_reservations": [
            {
              "type": "DYNAMIC",
              "role": "storage"
            }
          ],
          "storage": {
            "plugin": {
              "type": "org.apache.mesos.csi.test",
              "name": "slrp_test",
              "containers": [
                {
                  "services": [
                    "CONTROLLER_SERVICE",
                    "NODE_SERVICE"
                  ],
                  "command": {
                    "shell": false,
                    "value": "%s",
                    "arguments": [
                      "%s",
                      "--available_capacity=%s",
                      "--work_dir=%s"
                    ]
                  }
                }
              ]
            }
          }
        }
        )~",
        testCsiPluginPath,
        testCsiPluginPath,
        stringify(capacity),
        testCsiPluginWorkDir);

    CHECK_SOME(resourceProviderConfig);

    Try<JSON::Object> json =
      JSON::parse<JSON::Object>(resourceProviderConfig.get());
    CHECK_SOME(json);

    Try<ResourceProviderInfo> info =
      ::protobuf::parse<ResourceProviderInfo>(json.get());
    CHECK_SOME(info);

    return info.get();
  }

  Future<http::Response> addResourceProviderConfig(
      const PID<Slave>& pid,
      const ContentType& contentType,
      const ResourceProviderInfo& info)
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    agent::Call call;
    call.set_type(agent::Call::ADD_RESOURCE_PROVIDER_CONFIG);
    call.mutable_add_resource_provider_config()
      ->mutable_info()->CopyFrom(info);

    return http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, evolve(call)),
        stringify(contentType));
  }

  Future<http::Response> updateResourceProviderConfig(
      const PID<Slave>& pid,
      const ContentType& contentType,
      const ResourceProviderInfo& info)
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    agent::Call call;
    call.set_type(agent::Call::UPDATE_RESOURCE_PROVIDER_CONFIG);
    call.mutable_update_resource_provider_config()
      ->mutable_info()->CopyFrom(info);

    return http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, evolve(call)),
        stringify(contentType));
  }

  Future<http::Response> removeResourceProviderConfig(
      const PID<Slave>& pid,
      const ContentType& contentType,
      const string& type,
      const string& name)
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    agent::Call call;
    call.set_type(agent::Call::REMOVE_RESOURCE_PROVIDER_CONFIG);
    call.mutable_remove_resource_provider_config()->set_type(type);
    call.mutable_remove_resource_provider_config()->set_name(name);

    return http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, evolve(call)),
        stringify(contentType));
  }

protected:
  string resourceProviderConfigDir;
};


// The tests are parameterized by the content type of the request.
INSTANTIATE_TEST_CASE_P(
    ContentType,
    AgentResourceProviderConfigApiTest,
    Values(ContentType::PROTOBUF, ContentType::JSON));


// This test adds a new resource provider config on the fly.
TEST_P(AgentResourceProviderConfigApiTest, ROOT_Add)
{
  const ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  flags.authenticate_http_readwrite = false;

  // Set the resource provider capability and other required capabilities.
  constexpr SlaveInfo::Capability::Type capabilities[] = {
    SlaveInfo::Capability::MULTI_ROLE,
    SlaveInfo::Capability::HIERARCHICAL_ROLE,
    SlaveInfo::Capability::RESERVATION_REFINEMENT,
    SlaveInfo::Capability::RESOURCE_PROVIDER
  };

  flags.agent_features = SlaveCapabilities();
  foreach (SlaveInfo::Capability::Type type, capabilities) {
    flags.agent_features->add_capabilities()->set_type(type);
  }

  flags.resource_provider_config_dir = resourceProviderConfigDir;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to wait for an offer having the provider
  // resource.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(&Resources::hasResourceProvider, lambda::_1))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // Add a new resource provider.
  ResourceProviderInfo info = createResourceProviderInfo(Gigabytes(4));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::OK().status,
      addResourceProviderConfig(slave.get()->pid, contentType, info));

  // Check that a new config file is created.
  Try<list<string>> configPaths =
    fs::list(path::join(resourceProviderConfigDir, "*"));
  ASSERT_SOME(configPaths);
  EXPECT_EQ(1u, configPaths->size());

  Try<string> read = os::read(configPaths->back());
  ASSERT_SOME(read);

  Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
  ASSERT_SOME(json);

  Try<ResourceProviderInfo> _info =
    ::protobuf::parse<ResourceProviderInfo>(json.get());
  ASSERT_SOME(_info);
  EXPECT_EQ(_info.get(), info);

  // Wait for an offer having the provider resource.
  AWAIT_READY(offers);
}


// This test checks that adding a resource provider config that already
// exists is not allowed.
TEST_P(AgentResourceProviderConfigApiTest, ROOT_AddConflict)
{
  const ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  flags.authenticate_http_readwrite = false;

  // Set the resource provider capability and other required capabilities.
  constexpr SlaveInfo::Capability::Type capabilities[] = {
    SlaveInfo::Capability::MULTI_ROLE,
    SlaveInfo::Capability::HIERARCHICAL_ROLE,
    SlaveInfo::Capability::RESERVATION_REFINEMENT,
    SlaveInfo::Capability::RESOURCE_PROVIDER
  };

  flags.agent_features = SlaveCapabilities();
  foreach (SlaveInfo::Capability::Type type, capabilities) {
    flags.agent_features->add_capabilities()->set_type(type);
  }

  flags.resource_provider_config_dir = resourceProviderConfigDir;

  // Generate a pre-existing config.
  const string configPath = path::join(resourceProviderConfigDir, "test.json");
  ASSERT_SOME(os::write(
      configPath,
      stringify(JSON::protobuf(createResourceProviderInfo(Gigabytes(4))))));

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  ResourceProviderInfo info = createResourceProviderInfo(Gigabytes(2));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::Conflict().status,
      addResourceProviderConfig(slave.get()->pid, contentType, info));

  // Check that no new config is created, and the existing one is not
  // overwritten.
  Try<list<string>> configPaths =
    fs::list(path::join(resourceProviderConfigDir, "*"));
  ASSERT_SOME(configPaths);
  EXPECT_EQ(1u, configPaths->size());
  EXPECT_EQ(configPath, configPaths->back());

  Try<string> read = os::read(configPath);
  ASSERT_SOME(read);

  Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
  ASSERT_SOME(json);

  Try<ResourceProviderInfo> _info =
    ::protobuf::parse<ResourceProviderInfo>(json.get());
  ASSERT_SOME(_info);
  EXPECT_NE(_info.get(), info);
}


// This test updates an existing resource provider config on the fly.
TEST_P(AgentResourceProviderConfigApiTest, ROOT_Update)
{
  const ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  flags.authenticate_http_readwrite = false;

  // Set the resource provider capability and other required capabilities.
  constexpr SlaveInfo::Capability::Type capabilities[] = {
    SlaveInfo::Capability::MULTI_ROLE,
    SlaveInfo::Capability::HIERARCHICAL_ROLE,
    SlaveInfo::Capability::RESERVATION_REFINEMENT,
    SlaveInfo::Capability::RESOURCE_PROVIDER
  };

  flags.agent_features = SlaveCapabilities();
  foreach (SlaveInfo::Capability::Type type, capabilities) {
    flags.agent_features->add_capabilities()->set_type(type);
  }

  flags.resource_provider_config_dir = resourceProviderConfigDir;

  // Generate a pre-existing config.
  const string configPath = path::join(resourceProviderConfigDir, "test.json");
  ASSERT_SOME(os::write(
      configPath,
      stringify(JSON::protobuf(createResourceProviderInfo(Gigabytes(4))))));

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to wait for an offer having the provider
  // resource.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> oldOffers;
  Future<vector<Offer>> newOffers;

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(&Resources::hasResourceProvider, lambda::_1))))
    .WillOnce(FutureArg<1>(&oldOffers))
    .WillOnce(FutureArg<1>(&newOffers));

  Future<OfferID> rescinded;

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureArg<1>(&rescinded));

  driver.start();

  // Wait for an offer having the old provider resource.
  AWAIT_READY(oldOffers);

  ResourceProviderInfo info = createResourceProviderInfo(Gigabytes(2));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::OK().status,
      updateResourceProviderConfig(slave.get()->pid, contentType, info));

  // Check that no new config is created, and the existing one is overwritten.
  Try<list<string>> configPaths =
    fs::list(path::join(resourceProviderConfigDir, "*"));
  ASSERT_SOME(configPaths);
  EXPECT_EQ(1u, configPaths->size());
  EXPECT_EQ(configPath, configPaths->back());

  Try<string> read = os::read(configPath);
  ASSERT_SOME(read);

  Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
  ASSERT_SOME(json);

  Try<ResourceProviderInfo> _info =
    ::protobuf::parse<ResourceProviderInfo>(json.get());
  ASSERT_SOME(_info);
  EXPECT_EQ(_info.get(), info);

  // Wait for the old offer to be rescinded.
  AWAIT_READY(rescinded);

  // Wait for an offer having the new provider resource.
  AWAIT_READY(newOffers);

  // The new provider resource is smaller than the old provider resource.
  EXPECT_FALSE(Resources(newOffers->at(0).resources()).contains(
      oldOffers->at(0).resources()));
}


// This test checks that updating a nonexistent resource provider config
// is not allowed.
TEST_P(AgentResourceProviderConfigApiTest, UpdateNotFound)
{
  const ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();

  // Set the resource provider capability and other required capabilities.
  constexpr SlaveInfo::Capability::Type capabilities[] = {
    SlaveInfo::Capability::MULTI_ROLE,
    SlaveInfo::Capability::HIERARCHICAL_ROLE,
    SlaveInfo::Capability::RESERVATION_REFINEMENT,
    SlaveInfo::Capability::RESOURCE_PROVIDER
  };

  flags.agent_features = SlaveCapabilities();
  foreach (SlaveInfo::Capability::Type type, capabilities) {
    flags.agent_features->add_capabilities()->set_type(type);
  }

  flags.resource_provider_config_dir = resourceProviderConfigDir;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  ResourceProviderInfo info = createResourceProviderInfo(Gigabytes(4));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::NotFound().status,
      updateResourceProviderConfig(slave.get()->pid, contentType, info));

  // Check that no new config is created.
  Try<list<string>> configPaths =
    fs::list(path::join(resourceProviderConfigDir, "*"));
  ASSERT_SOME(configPaths);
  EXPECT_TRUE(configPaths->empty());
}


// This test removes an existing resource provider config on the fly.
TEST_P(AgentResourceProviderConfigApiTest, ROOT_Remove)
{
  const ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  flags.authenticate_http_readwrite = false;

  // Set the resource provider capability and other required capabilities.
  constexpr SlaveInfo::Capability::Type capabilities[] = {
    SlaveInfo::Capability::MULTI_ROLE,
    SlaveInfo::Capability::HIERARCHICAL_ROLE,
    SlaveInfo::Capability::RESERVATION_REFINEMENT,
    SlaveInfo::Capability::RESOURCE_PROVIDER
  };

  flags.agent_features = SlaveCapabilities();
  foreach (SlaveInfo::Capability::Type type, capabilities) {
    flags.agent_features->add_capabilities()->set_type(type);
  }

  flags.resource_provider_config_dir = resourceProviderConfigDir;

  // Generate a pre-existing config.
  const string configPath = path::join(resourceProviderConfigDir, "test.json");
  ResourceProviderInfo info = createResourceProviderInfo(Gigabytes(4));
  ASSERT_SOME(os::write(configPath, stringify(JSON::protobuf(info))));

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to wait for an offer having the provider
  // resource.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> oldOffers;

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(&Resources::hasResourceProvider, lambda::_1))))
    .WillOnce(FutureArg<1>(&oldOffers));

  // TODO(chhsiao): Wait for an rescinded offer once we implemented the
  // logic to send `UpdateSlaveMessage` upon removal of a resource
  // provider.

  driver.start();

  // Wait for an offer having the old provider resource.
  AWAIT_READY(oldOffers);

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::OK().status,
      removeResourceProviderConfig(
          slave.get()->pid, contentType, info.type(), info.name()));

  // Check that the existing config is removed.
  EXPECT_FALSE(os::exists(configPath));

  // TODO(chhsiao): Wait for the old offer to be rescinded.
}


// This test checks that removing a nonexistent resource provider config
// is not allowed.
TEST_P(AgentResourceProviderConfigApiTest, RemoveNotFound)
{
  const ContentType contentType = GetParam();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();

  // Set the resource provider capability and other required capabilities.
  constexpr SlaveInfo::Capability::Type capabilities[] = {
    SlaveInfo::Capability::MULTI_ROLE,
    SlaveInfo::Capability::HIERARCHICAL_ROLE,
    SlaveInfo::Capability::RESERVATION_REFINEMENT,
    SlaveInfo::Capability::RESOURCE_PROVIDER
  };

  flags.agent_features = SlaveCapabilities();
  foreach (SlaveInfo::Capability::Type type, capabilities) {
    flags.agent_features->add_capabilities()->set_type(type);
  }

  flags.resource_provider_config_dir = resourceProviderConfigDir;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  ResourceProviderInfo info = createResourceProviderInfo(Gigabytes(4));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      http::NotFound().status,
      removeResourceProviderConfig(
          slave.get()->pid, contentType, info.type(), info.name()));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
