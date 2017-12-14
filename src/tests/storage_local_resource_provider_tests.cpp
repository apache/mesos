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

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/gmock.hpp>

#include <stout/hashmap.hpp>

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using std::shared_ptr;
using std::string;
using std::vector;

using mesos::master::detector::MasterDetector;

using process::Future;
using process::Owned;

using testing::Args;
using testing::Sequence;

namespace mesos {
namespace internal {
namespace tests {

class StorageLocalResourceProviderTest : public MesosTest
{
public:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    const string testCsiPluginWorkDir = path::join(sandbox.get(), "test");
    ASSERT_SOME(os::mkdir(testCsiPluginWorkDir));

    resourceProviderConfigDir =
      path::join(sandbox.get(), "resource_provider_configs");

    ASSERT_SOME(os::mkdir(resourceProviderConfigDir));

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
                      "--available_capacity=2GB",
                      "--volumes=volume1:1GB;volume2:1GB",
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
        testCsiPluginWorkDir);

    ASSERT_SOME(resourceProviderConfig);

    ASSERT_SOME(os::write(
        path::join(resourceProviderConfigDir, "test.json"),
        resourceProviderConfig.get()));
  }

protected:
  string resourceProviderConfigDir;
};


// This test verifies that a framework can create then destroy a new
// volume from the storage pool of a storage local resource provider
// that uses the test CSI plugin.
TEST_F(StorageLocalResourceProviderTest, ROOT_NewVolume)
{
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

  // Register a framework to exercise offer operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing a RAW disk resource before `CREATE_VOLUME`.
  //   2. One containing a MOUNT disk resource after `CREATE_VOLUME`.
  //   3. One containing a RAW disk resource after `DSTROY_VOLUME`.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> volumeCreatedOffers;
  Future<vector<Offer>> volumeDestroyedOffers;

  Sequence offers;

  // We are only interested in storage pools and volume created from
  // them, which have a "default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "default" &&
      r.disk().source().type() == type;
  };

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::RAW))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&rawDiskOffers));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::MOUNT))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeCreatedOffers));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::RAW))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeDestroyedOffers));

  driver.start();

  AWAIT_READY(rawDiskOffers);
  ASSERT_FALSE(rawDiskOffers->empty());

  Option<Resource> source;

  foreach (const Resource& resource, rawDiskOffers->at(0).resources()) {
    if (hasSourceType(resource, Resource::DiskInfo::Source::RAW)) {
      source = resource;
      break;
    }
  }

  ASSERT_SOME(source);

  // Create a volume.
  driver.acceptOffers(
      {rawDiskOffers->at(0).id()},
      {CREATE_VOLUME(source.get(), Resource::DiskInfo::Source::MOUNT)},
      filters);

  AWAIT_READY(volumeCreatedOffers);
  ASSERT_FALSE(volumeCreatedOffers->empty());

  Option<Resource> volume;

  foreach (const Resource& resource, volumeCreatedOffers->at(0).resources()) {
    if (hasSourceType(resource, Resource::DiskInfo::Source::MOUNT)) {
      volume = resource;
      break;
    }
  }

  ASSERT_SOME(volume);
  ASSERT_TRUE(volume->disk().source().has_id());
  ASSERT_TRUE(volume->disk().source().has_metadata());
  ASSERT_TRUE(volume->disk().source().has_mount());
  ASSERT_TRUE(volume->disk().source().mount().has_root());
  EXPECT_FALSE(path::absolute(volume->disk().source().mount().root()));

  // Check if the volume is actually created by the test CSI plugin.
  Option<string> volumePath;

  foreach (const Label& label, volume->disk().source().metadata().labels()) {
    if (label.key() == "path") {
      volumePath = label.value();
      break;
    }
  }

  ASSERT_SOME(volumePath);
  EXPECT_TRUE(os::exists(volumePath.get()));

  // Destroy the created volume.
  driver.acceptOffers(
      {volumeCreatedOffers->at(0).id()},
      {DESTROY_VOLUME(volume.get())},
      filters);

  AWAIT_READY(volumeDestroyedOffers);
  ASSERT_FALSE(volumeDestroyedOffers->empty());

  Option<Resource> destroyed;

  foreach (const Resource& resource, volumeDestroyedOffers->at(0).resources()) {
    if (hasSourceType(resource, Resource::DiskInfo::Source::RAW)) {
      destroyed = resource;
      break;
    }
  }

  ASSERT_SOME(destroyed);
  ASSERT_FALSE(destroyed->disk().source().has_id());
  ASSERT_FALSE(destroyed->disk().source().has_metadata());
  ASSERT_FALSE(destroyed->disk().source().has_mount());

  // Check if the volume is actually deleted by the test CSI plugin.
  EXPECT_FALSE(os::exists(volumePath.get()));
}


// This test verifies that a framework can launch a task using a created
// volume from a storage local resource provider that uses the test CSI
// plugin, then destroy the volume while it is published.
TEST_F(StorageLocalResourceProviderTest, ROOT_LaunchTask)
{
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

  // Register a framework to exercise offer operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing a RAW disk resource before `CREATE_VOLUME`.
  //   2. One containing a MOUNT disk resource after `CREATE_VOLUME`.
  //   3. One containing the same MOUNT disk resource after `CREADE`,
  //      `LAUNCH` and `DESTROY`.
  //   4. One containing the same RAW disk resource after `DESTROY_VOLUME`.
  //
  // We set up the expectations for these offers as the test progresses.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> volumeCreatedOffers;
  Future<vector<Offer>> taskFinishedOffers;
  Future<vector<Offer>> volumeDestroyedOffers;

  Sequence offers;

  // We are only interested in storage pools and volume created from
  // them, which have a "default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "default" &&
      r.disk().source().type() == type;
  };

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::RAW))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&rawDiskOffers));

  driver.start();

  AWAIT_READY(rawDiskOffers);
  ASSERT_FALSE(rawDiskOffers->empty());

  Option<Resource> source;

  foreach (const Resource& resource, rawDiskOffers->at(0).resources()) {
    if (hasSourceType(resource, Resource::DiskInfo::Source::RAW)) {
      source = resource;
      break;
    }
  }

  ASSERT_SOME(source);

  // Create a volume.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::MOUNT))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeCreatedOffers));

  driver.acceptOffers(
      {rawDiskOffers->at(0).id()},
      {CREATE_VOLUME(source.get(), Resource::DiskInfo::Source::MOUNT)},
      filters);

  AWAIT_READY(volumeCreatedOffers);
  ASSERT_FALSE(volumeCreatedOffers->empty());

  Option<Resource> volume;

  foreach (const Resource& resource, volumeCreatedOffers->at(0).resources()) {
    if (hasSourceType(resource, Resource::DiskInfo::Source::MOUNT)) {
      volume = resource;
      break;
    }
  }

  ASSERT_SOME(volume);
  ASSERT_TRUE(volume->disk().source().has_id());
  ASSERT_TRUE(volume->disk().source().has_metadata());
  ASSERT_TRUE(volume->disk().source().has_mount());
  ASSERT_TRUE(volume->disk().source().mount().has_root());
  EXPECT_FALSE(path::absolute(volume->disk().source().mount().root()));

  // Check if the volume is actually created by the test CSI plugin.
  Option<string> volumePath;

  foreach (const Label& label, volume->disk().source().metadata().labels()) {
    if (label.key() == "path") {
      volumePath = label.value();
      break;
    }
  }

  ASSERT_SOME(volumePath);
  EXPECT_TRUE(os::exists(volumePath.get()));

  // Put a file into the volume.
  ASSERT_SOME(os::touch(path::join(volumePath.get(), "file")));

  // Create a persistent volume on the CSI volume, then launch a task to
  // use the persistent volume.
  Resource persistentVolume = volume.get();
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_id(id::UUID::random().toString());
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_principal(framework.principal());
  persistentVolume.mutable_disk()->mutable_volume()
    ->set_container_path("volume");
  persistentVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

  Future<TaskStatus> taskStarting;
  Future<TaskStatus> taskRunning;
  Future<TaskStatus> taskFinished;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&taskStarting))
    .WillOnce(FutureArg<1>(&taskRunning))
    .WillOnce(FutureArg<1>(&taskFinished));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(
      persistentVolume)))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&taskFinishedOffers));

  driver.acceptOffers(
      {volumeCreatedOffers->at(0).id()},
      {CREATE(persistentVolume),
       LAUNCH({createTask(
           volumeCreatedOffers->at(0).slave_id(),
           persistentVolume,
           createCommandInfo("test -f " + path::join("volume", "file")))})},
      filters);

  AWAIT_READY(taskStarting);
  EXPECT_EQ(TASK_STARTING, taskStarting->state());

  AWAIT_READY(taskRunning);
  EXPECT_EQ(TASK_RUNNING, taskRunning->state());

  AWAIT_READY(taskFinished);
  EXPECT_EQ(TASK_FINISHED, taskFinished->state());

  AWAIT_READY(taskFinishedOffers);

  // Destroy the persistent volume and the CSI volume.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(source.get())))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeDestroyedOffers));

  driver.acceptOffers(
      {taskFinishedOffers->at(0).id()},
      {DESTROY(persistentVolume),
       DESTROY_VOLUME(volume.get())},
      filters);

  AWAIT_READY(volumeDestroyedOffers);
  ASSERT_FALSE(volumeDestroyedOffers->empty());

  // Check if the volume is actually deleted by the test CSI plugin.
  EXPECT_FALSE(os::exists(volumePath.get()));
}


// This test verifies that a framework can convert pre-existing volumes
// from a storage local resource provider that uses the test CSI plugin
// into mount or block volumes.
TEST_F(StorageLocalResourceProviderTest, ROOT_PreExistingVolume)
{
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

  // Register a framework to exercise offer operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing two RAW disk resources for pre-existing volumes
  //      before `CREATE_VOLUME` and `CREATE_BLOCK`.
  //   2. One containing a MOUNT and a BLOCK disk resources after
  //      `CREATE_VOLUME` and `CREATE_BLOCK`.
  //   3. One containing two RAW disk resources for pre-existing volumes
  //      resource after `DSTROY_VOLUME` and `DESTROY_BLOCK`.
  Future<vector<Offer>> rawDisksOffers;
  Future<vector<Offer>> disksConvertedOffers;
  Future<vector<Offer>> disksRevertedOffers;

  // We are only interested in pre-existing volumes, which have IDs but
  // no profile.
  auto isPreExistingVolume = [](const Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_id() &&
      !r.disk().source().has_profile();
  };

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      isPreExistingVolume)))
    .WillOnce(FutureArg<1>(&rawDisksOffers))
    .WillOnce(FutureArg<1>(&disksConvertedOffers))
    .WillOnce(FutureArg<1>(&disksRevertedOffers));

  driver.start();

  AWAIT_READY(rawDisksOffers);
  ASSERT_FALSE(rawDisksOffers->empty());

  vector<Resource> sources;

  foreach (const Resource& resource, rawDisksOffers->at(0).resources()) {
    if (isPreExistingVolume(resource) &&
        resource.disk().source().type() == Resource::DiskInfo::Source::RAW) {
      sources.push_back(resource);
    }
  }

  ASSERT_EQ(2u, sources.size());

  // Create a volume and a block.
  driver.acceptOffers(
      {rawDisksOffers->at(0).id()},
      {CREATE_VOLUME(sources.at(0), Resource::DiskInfo::Source::MOUNT),
       CREATE_BLOCK(sources.at(1))},
      filters);

  AWAIT_READY(disksConvertedOffers);
  ASSERT_FALSE(disksConvertedOffers->empty());

  Option<Resource> volume;
  Option<Resource> block;

  foreach (const Resource& resource, disksConvertedOffers->at(0).resources()) {
    if (isPreExistingVolume(resource)) {
      if (resource.disk().source().type() ==
            Resource::DiskInfo::Source::MOUNT) {
        volume = resource;
      } else if (resource.disk().source().type() ==
                   Resource::DiskInfo::Source::BLOCK) {
        block = resource;
      }
    }
  }

  ASSERT_SOME(volume);
  ASSERT_TRUE(volume->disk().source().has_mount());
  ASSERT_TRUE(volume->disk().source().mount().has_root());
  EXPECT_FALSE(path::absolute(volume->disk().source().mount().root()));

  ASSERT_SOME(block);

  // Destroy the created volume.
  driver.acceptOffers(
      {disksConvertedOffers->at(0).id()},
      {DESTROY_VOLUME(volume.get()),
       DESTROY_BLOCK(block.get())},
      filters);

  AWAIT_READY(disksRevertedOffers);
  ASSERT_FALSE(disksRevertedOffers->empty());

  vector<Resource> destroyed;

  foreach (const Resource& resource, disksRevertedOffers->at(0).resources()) {
    if (isPreExistingVolume(resource) &&
        resource.disk().source().type() == Resource::DiskInfo::Source::RAW) {
      destroyed.push_back(resource);
    }
  }

  ASSERT_EQ(2u, destroyed.size());

  foreach (const Resource& resource, destroyed) {
    ASSERT_FALSE(resource.disk().source().has_mount());
    ASSERT_TRUE(resource.disk().source().has_metadata());

    // Check if the volume is not deleted by the test CSI plugin.
    Option<string> volumePath;

    foreach (const Label& label, resource.disk().source().metadata().labels()) {
      if (label.key() == "path") {
        volumePath = label.value();
        break;
      }
    }

    ASSERT_SOME(volumePath);
    EXPECT_TRUE(os::exists(volumePath.get()));
  }
}


} // namespace tests {
} // namespace internal {
} // namespace mesos {
