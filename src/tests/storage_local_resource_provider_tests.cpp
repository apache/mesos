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

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/gmock.hpp>

#include <stout/hashmap.hpp>
#include <stout/uri.hpp>

#include "module/manager.hpp"

#include "slave/container_daemon_process.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using std::shared_ptr;
using std::string;
using std::vector;

using mesos::internal::slave::ContainerDaemonProcess;

using mesos::master::detector::MasterDetector;

using mesos::v1::resource_provider::Call;

using process::Clock;
using process::Future;
using process::Owned;

using testing::Args;
using testing::AtLeast;
using testing::Sequence;

namespace mesos {
namespace internal {
namespace tests {

constexpr char URI_DISK_PROFILE_ADAPTOR_NAME[] =
  "org_apache_mesos_UriDiskProfileAdaptor";


class StorageLocalResourceProviderTest : public MesosTest
{
public:
  virtual void SetUp()
  {
    MesosTest::SetUp();

    testCsiPluginWorkDir = path::join(sandbox.get(), "test");
    ASSERT_SOME(os::mkdir(testCsiPluginWorkDir));

    resourceProviderConfigDir =
      path::join(sandbox.get(), "resource_provider_configs");
    ASSERT_SOME(os::mkdir(resourceProviderConfigDir));

    uriDiskProfileConfigPath =
      path::join(sandbox.get(), "disk_profiles.json");
  }

  virtual void TearDown()
  {
    // Unload modules.
    foreach (const Modules::Library& library, modules.libraries()) {
      foreach (const Modules::Library::Module& module, library.modules()) {
        if (module.has_name()) {
          ASSERT_SOME(modules::ModuleManager::unload(module.name()));
        }
      }
    }

    MesosTest::TearDown();
  }

  void loadUriDiskProfileModule()
  {
    const string libraryPath = getModulePath("uri_disk_profile");

    Modules::Library* library = modules.add_libraries();
    library->set_name("uri_disk_profile");
    library->set_file(libraryPath);

    Modules::Library::Module* module = library->add_modules();
    module->set_name(URI_DISK_PROFILE_ADAPTOR_NAME);

    Parameter* uri = module->add_parameters();
    uri->set_key("uri");
    uri->set_value(uriDiskProfileConfigPath);
    Parameter* pollInterval = module->add_parameters();
    pollInterval->set_key("poll_interval");
    pollInterval->set_value("1secs");

    ASSERT_SOME(modules::ModuleManager::load(modules));
  }

  void setupResourceProviderConfig(
      const Bytes& capacity,
      const Option<string> volumes = None())
  {
    const string testCsiPluginPath =
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
                      "--volumes=%s",
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
        volumes.getOrElse(""),
        testCsiPluginWorkDir);

    ASSERT_SOME(resourceProviderConfig);

    ASSERT_SOME(os::write(
        path::join(resourceProviderConfigDir, "test.json"),
        resourceProviderConfig.get()));
  }

  void setupDiskProfileConfig()
  {
    Try<Nothing> write = os::write(
        uriDiskProfileConfigPath,
        R"~(
        {
          "profile_matrix": {
            "volume-default": {
              "volume_capabilities": {
                "mount": {},
                "access_mode": {
                  "mode": "SINGLE_NODE_WRITER"
                }
              }
            },
            "block-default": {
              "volume_capabilities": {
                "block": {},
                "access_mode": {
                  "mode": "SINGLE_NODE_WRITER"
                }
              }
            }
          }
        }
        )~");

    ASSERT_SOME(write);
  }

protected:
  Modules modules;
  string resourceProviderConfigDir;
  string testCsiPluginWorkDir;
  string uriDiskProfileConfigPath;
};


// This test verifies that a storage local resource provider can report
// no resource and recover from this state.
TEST_F(StorageLocalResourceProviderTest, ROOT_NoResource)
{
  Clock::pause();

  setupResourceProviderConfig(Bytes(0));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  // NOTE: The order of the two `FUTURE_PROTOBUF`s are reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlave2 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);
  Future<UpdateSlaveMessage> updateSlave1 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration and prevent retry.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlave1);

  // NOTE: We need to resume the clock so that the resource provider can
  // periodically check if the CSI endpoint socket has been created by
  // the plugin container, which runs in another Linux process.
  Clock::resume();

  AWAIT_READY(updateSlave2);
  ASSERT_TRUE(updateSlave2->has_resource_providers());
  ASSERT_EQ(1, updateSlave2->resource_providers().providers_size());
  EXPECT_EQ(
      0,
      updateSlave2->resource_providers().providers(0).total_resources_size());

  Clock::pause();

  // Restart the agent.
  slave.get()->terminate();

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  // NOTE: The order of the two `FUTURE_PROTOBUF`s are reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlave4 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);
  Future<UpdateSlaveMessage> updateSlave3 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration and prevent retry.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlave3);

  Clock::resume();

  AWAIT_READY(updateSlave4);
  ASSERT_TRUE(updateSlave4->has_resource_providers());
  ASSERT_EQ(1, updateSlave4->resource_providers().providers_size());
  EXPECT_EQ(
      0,
      updateSlave4->resource_providers().providers(0).total_resources_size());
}


// This test verifies that any zero-sized volume reported by a CSI
// plugin will be ignored by the storage local resource provider.
TEST_F(StorageLocalResourceProviderTest, ROOT_ZeroSizedDisk)
{
  Clock::pause();

  setupResourceProviderConfig(Bytes(0), "volume0:0B");

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  // NOTE: The order of the two `FUTURE_PROTOBUF`s are reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlave2 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);
  Future<UpdateSlaveMessage> updateSlave1 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration and prevent retry.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlave1);

  Clock::resume();

  AWAIT_READY(updateSlave2);
  ASSERT_TRUE(updateSlave2->has_resource_providers());
  ASSERT_EQ(1, updateSlave2->resource_providers().providers_size());

  Option<Resource> volume;
  foreach (const Resource& resource,
           updateSlave2->resource_providers().providers(0).total_resources()) {
    if (Resources::hasResourceProvider(resource)) {
      volume = resource;
    }
  }

  ASSERT_NONE(volume);
}


// This test verifies that the storage local resource provider can
// handle disks less than 1MB correctly.
TEST_F(StorageLocalResourceProviderTest, ROOT_SmallDisk)
{
  loadUriDiskProfileModule();

  setupResourceProviderConfig(Kilobytes(512), "volume0:512KB");
  setupDiskProfileConfig();

  master::Flags masterFlags = CreateMasterFlags();

  // Use a small allocation interval to speed up the test. We do this
  // instead of manipulating the clock to keep the test concise and
  // avoid waiting for `UpdateSlaveMessage`s and pausing/resuming the
  // clock multiple times.
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to receive offers.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> rawDisksOffers;

  // We are interested in offers that contains both the storage pool and
  // the pre-existing volume.
  auto isStoragePool = [](const Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().type() == Resource::DiskInfo::Source::RAW &&
      !r.disk().source().has_id() &&
      r.disk().source().has_profile();
  };

  auto isPreExistingVolume = [](const Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_id() &&
      !r.disk().source().has_profile();
  };

  // Since the master may send out offers before the resource provider
  // reports the storage pool, we decline offers that do not have any
  // storage pool. This would also decline offers that contain only the
  // agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      isStoragePool)))
    .WillOnce(FutureArg<1>(&rawDisksOffers));

  driver.start();

  AWAIT_READY(rawDisksOffers);
  ASSERT_FALSE(rawDisksOffers->empty());

  Option<Resource> storagePool;
  Option<Resource> preExistingVolume;
  foreach (const Resource& resource, rawDisksOffers->at(0).resources()) {
    if (isStoragePool(resource)) {
      storagePool = resource;
    } else if (isPreExistingVolume(resource)) {
      preExistingVolume = resource;
    }
  }

  ASSERT_SOME(storagePool);
  EXPECT_EQ(
      Kilobytes(512),
      Bytes(storagePool->scalar().value() * Bytes::MEGABYTES));

  ASSERT_SOME(preExistingVolume);
  EXPECT_EQ(
      Kilobytes(512),
      Bytes(preExistingVolume->scalar().value() * Bytes::MEGABYTES));
}


// This test verifies that a framework can receive offers having new
// storage pools from the storage local resource provider after a new
// profile appears.
TEST_F(StorageLocalResourceProviderTest, ROOT_NewProfile)
{
  Clock::pause();

  loadUriDiskProfileModule();

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  // NOTE: The order of the two `FUTURE_PROTOBUF`s are reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlave2 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);
  Future<UpdateSlaveMessage> updateSlave1 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration and prevent retry.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(updateSlave1);

  Clock::resume();

  // No resource should be reported by the resource provider before
  // adding any profile.
  AWAIT_READY(updateSlave2);
  ASSERT_TRUE(updateSlave2->has_resource_providers());
  ASSERT_EQ(1, updateSlave2->resource_providers().providers_size());
  EXPECT_EQ(
      0,
      updateSlave2->resource_providers().providers(0).total_resources_size());

  Future<UpdateSlaveMessage> updateSlave3 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // Add new profiles.
  setupDiskProfileConfig();

  // A new storage pool for profile "volume-default" should be reported
  // by the resource provider. Still expect no storage pool for
  // "block-default" since it is not supported by the test CSI plugin.
  AWAIT_READY(updateSlave3);
  ASSERT_TRUE(updateSlave3->has_resource_providers());
  ASSERT_EQ(1, updateSlave3->resource_providers().providers_size());
  EXPECT_EQ(
      1,
      updateSlave3->resource_providers().providers(0).total_resources_size());

  Option<Resource> volumeStoragePool;
  Option<Resource> blockStoragePool;
  foreach (const Resource& resource,
           updateSlave3->resource_providers().providers(0).total_resources()) {
    if (!resource.has_disk() ||
        !resource.disk().has_source() ||
        resource.disk().source().type() != Resource::DiskInfo::Source::RAW ||
        !resource.disk().source().has_profile() ||
        resource.disk().source().has_id()) {
      continue;
    }

    if (resource.disk().source().profile() == "volume-default") {
      volumeStoragePool = resource;
    } else if (resource.disk().source().profile() == "block-default") {
      blockStoragePool = resource;
    }
  }

  EXPECT_SOME(volumeStoragePool);
  EXPECT_NONE(blockStoragePool);
}


// This test verifies that the storage local resource provider can
// create then destroy a new volume from a storage pool.
TEST_F(StorageLocalResourceProviderTest, ROOT_CreateDestroyVolume)
{
  loadUriDiskProfileModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileConfig();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to exercise operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing a RAW disk resource before `CREATE_VOLUME`.
  //   2. One containing a MOUNT disk resource after `CREATE_VOLUME`.
  //   3. One containing a RAW disk resource after `DESTROY_VOLUME`.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> volumeCreatedOffers;
  Future<vector<Offer>> volumeDestroyedOffers;

  Sequence offers;

  // We are only interested in storage pools and volume created from
  // them, which have a "volume-default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "volume-default" &&
      r.disk().source().type() == type;
  };

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

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
      acceptFilters);

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
      acceptFilters);

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


// This test verifies that the storage local resource provider can
// destroy a volume created from a storage pool after recovery.
TEST_F(StorageLocalResourceProviderTest, ROOT_CreateDestroyVolumeRecovery)
{
  loadUriDiskProfileModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileConfig();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to exercise operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing a RAW disk resource before `CREATE_VOLUME`.
  //   2. One containing a MOUNT disk resource after `CREATE_VOLUME`.
  //   3. One containing a MOUNT disk resource after the agent recovers
  //      from a failover.
  //   4. One containing a RAW disk resource after `DESTROY_VOLUME`.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> volumeCreatedOffers;
  Future<vector<Offer>> agentRecoveredOffers;
  Future<vector<Offer>> volumeDestroyedOffers;

  Sequence offers;

  // We are only interested in storage pools and volume created from
  // them, which have a "volume-default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "volume-default" &&
      r.disk().source().type() == type;
  };

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::RAW))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&rawDiskOffers));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::MOUNT))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeCreatedOffers))
    .WillOnce(FutureArg<1>(&agentRecoveredOffers));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::RAW))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeDestroyedOffers));

  EXPECT_CALL(sched, offerRescinded(_, _))
    .Times(AtLeast(1));

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
      acceptFilters);

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

  // Restart the agent.
  slave.get()->terminate();

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(agentRecoveredOffers);
  ASSERT_FALSE(agentRecoveredOffers->empty());

  // Destroy the created volume.
  driver.acceptOffers(
      {agentRecoveredOffers->at(0).id()},
      {DESTROY_VOLUME(volume.get())},
      acceptFilters);

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


// This test verifies that the storage local resource provider can
// publish a volume required by a task, then destroy the published
// volume after the task finishes.
TEST_F(StorageLocalResourceProviderTest, ROOT_PublishResources)
{
  loadUriDiskProfileModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileConfig();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to exercise operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

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
  // them, which have a "volume-default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "volume-default" &&
      r.disk().source().type() == type;
  };

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

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
      acceptFilters);

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
      acceptFilters);

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
      acceptFilters);

  AWAIT_READY(volumeDestroyedOffers);
  ASSERT_FALSE(volumeDestroyedOffers->empty());

  // Check if the volume is actually deleted by the test CSI plugin.
  EXPECT_FALSE(os::exists(volumePath.get()));
}


// This test verifies that the storage local resource provider can
// destroy a published volume after recovery.
TEST_F(StorageLocalResourceProviderTest, ROOT_PublishResourcesRecovery)
{
  loadUriDiskProfileModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileConfig();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to exercise operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

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
  Future<vector<Offer>> agentRecoveredOffers;
  Future<vector<Offer>> volumeDestroyedOffers;

  Sequence offers;

  // We are only interested in storage pools and volume created from
  // them, which have a "volume-default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "volume-default" &&
      r.disk().source().type() == type;
  };

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::RAW))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&rawDiskOffers));

  EXPECT_CALL(sched, offerRescinded(_, _))
    .Times(AtLeast(1));

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
      acceptFilters);

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
    .WillOnce(FutureArg<1>(&taskFinishedOffers))
    .WillOnce(FutureArg<1>(&agentRecoveredOffers));

  driver.acceptOffers(
      {volumeCreatedOffers->at(0).id()},
      {CREATE(persistentVolume),
       LAUNCH({createTask(
           volumeCreatedOffers->at(0).slave_id(),
           persistentVolume,
           createCommandInfo("test -f " + path::join("volume", "file")))})},
      acceptFilters);

  AWAIT_READY(taskStarting);
  EXPECT_EQ(TASK_STARTING, taskStarting->state());

  AWAIT_READY(taskRunning);
  EXPECT_EQ(TASK_RUNNING, taskRunning->state());

  AWAIT_READY(taskFinished);
  EXPECT_EQ(TASK_FINISHED, taskFinished->state());

  AWAIT_READY(taskFinishedOffers);

  // Restart the agent.
  slave.get()->terminate();

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(agentRecoveredOffers);
  ASSERT_FALSE(agentRecoveredOffers->empty());

  // Destroy the persistent volume and the CSI volume.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(source.get())))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeDestroyedOffers));

  driver.acceptOffers(
      {agentRecoveredOffers->at(0).id()},
      {DESTROY(persistentVolume),
       DESTROY_VOLUME(volume.get())},
      acceptFilters);

  AWAIT_READY(volumeDestroyedOffers);
  ASSERT_FALSE(volumeDestroyedOffers->empty());

  // Check if the volume is actually deleted by the test CSI plugin.
  EXPECT_FALSE(os::exists(volumePath.get()));
}


// This test verifies that the storage local resource provider can
// restart its CSI plugin after it is killed and continue to work
// properly.
TEST_F(
    StorageLocalResourceProviderTest,
    ROOT_PublishUnpublishResourcesPluginKilled)
{
  loadUriDiskProfileModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileConfig();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  slave::Fetcher fetcher(slaveFlags);

  Try<slave::MesosContainerizer*> _containerizer =
    slave::MesosContainerizer::create(slaveFlags, false, &fetcher);
  ASSERT_SOME(_containerizer);

  Owned<slave::MesosContainerizer> containerizer(_containerizer.get());

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to exercise operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing a RAW disk resource before `CREATE_VOLUME`.
  //   2. One containing a MOUNT disk resource after `CREATE_VOLUME`.
  //   3. One containing the same MOUNT disk resource after `CREATE`,
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
  // them, which have a "volume-default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "volume-default" &&
      r.disk().source().type() == type;
  };

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

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

  // Get the ID of the CSI plugin container.
  Future<hashset<ContainerID>> pluginContainers = containerizer->containers();

  AWAIT_READY(pluginContainers);
  ASSERT_EQ(1u, pluginContainers->size());

  const ContainerID& pluginContainerId = *pluginContainers->begin();

  Future<Nothing> pluginRestarted =
    FUTURE_DISPATCH(_, &ContainerDaemonProcess::launchContainer);

  // Kill the plugin container and wait for it to restart.
  Future<int> pluginKilled = containerizer->status(pluginContainerId)
    .then([](const ContainerStatus& status) {
      return os::kill(status.executor_pid(), SIGKILL);
    });

  AWAIT_ASSERT_EQ(0, pluginKilled);
  AWAIT_READY(pluginRestarted);

  // Create a volume.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::MOUNT))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeCreatedOffers));

  driver.acceptOffers(
      {rawDiskOffers->at(0).id()},
      {CREATE_VOLUME(source.get(), Resource::DiskInfo::Source::MOUNT)},
      acceptFilters);

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

  pluginRestarted =
    FUTURE_DISPATCH(_, &ContainerDaemonProcess::launchContainer);

  // Kill the plugin container and wait for it to restart.
  pluginKilled = containerizer->status(pluginContainerId)
    .then([](const ContainerStatus& status) {
      return os::kill(status.executor_pid(), SIGKILL);
    });

  AWAIT_ASSERT_EQ(0, pluginKilled);
  AWAIT_READY(pluginRestarted);

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
      acceptFilters);

  AWAIT_READY(taskStarting);
  EXPECT_EQ(TASK_STARTING, taskStarting->state());

  AWAIT_READY(taskRunning);
  EXPECT_EQ(TASK_RUNNING, taskRunning->state());

  AWAIT_READY(taskFinished);
  EXPECT_EQ(TASK_FINISHED, taskFinished->state());

  AWAIT_READY(taskFinishedOffers);

  pluginRestarted =
    FUTURE_DISPATCH(_, &ContainerDaemonProcess::launchContainer);

  // Kill the plugin container and wait for it to restart.
  pluginKilled = containerizer->status(pluginContainerId)
    .then([](const ContainerStatus& status) {
      return os::kill(status.executor_pid(), SIGKILL);
    });

  AWAIT_ASSERT_EQ(0, pluginKilled);
  AWAIT_READY(pluginRestarted);

  // Destroy the persistent volume and the CSI volume.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(source.get())))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeDestroyedOffers));

  driver.acceptOffers(
      {taskFinishedOffers->at(0).id()},
      {DESTROY(persistentVolume),
       DESTROY_VOLUME(volume.get())},
      acceptFilters);

  AWAIT_READY(volumeDestroyedOffers);
  ASSERT_FALSE(volumeDestroyedOffers->empty());

  // Check if the volume is actually deleted by the test CSI plugin.
  EXPECT_FALSE(os::exists(volumePath.get()));
}


// This test verifies that the storage local resource provider can
// convert pre-existing CSI volumes into mount or block volumes.
TEST_F(StorageLocalResourceProviderTest, ROOT_ConvertPreExistingVolume)
{
  setupResourceProviderConfig(Bytes(0), "volume1:2GB;volume2:2GB");

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  slaveFlags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  slaveFlags.agent_features = SlaveCapabilities();
  slaveFlags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to exercise operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing two RAW disk resources for pre-existing volumes
  //      before `CREATE_VOLUME` and `CREATE_BLOCK`.
  //   2. One containing a MOUNT and a BLOCK disk resources after
  //      `CREATE_VOLUME` and `CREATE_BLOCK`.
  //   3. One containing two RAW disk resources for pre-existing volumes
  //      resource after `DESTROY_VOLUME` and `DESTROY_BLOCK`.
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
    .WillRepeatedly(DeclineOffers(declineFilters));

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
      acceptFilters);

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
      acceptFilters);

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


// This test verifies that operation status updates are resent to the master
// after being dropped en route to it.
//
// To accomplish this:
//   1. Creates a volume from a RAW disk resource.
//   2. Drops the first `UpdateOperationStatusMessage` from the agent to the
//      master, so that it isn't acknowledged by the master.
//   3. Advances the clock and verifies that the agent resends the operation
//      status update.
TEST_F(StorageLocalResourceProviderTest, ROOT_RetryOperationStatusUpdate)
{
  Clock::pause();

  loadUriDiskProfileModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileConfig();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  flags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  flags.agent_features = SlaveCapabilities();
  flags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  flags.resource_provider_config_dir = resourceProviderConfigDir;
  flags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  //
  // NOTE: The order of the two `FUTURE_PROTOBUF`s are reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlave2 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);
  Future<UpdateSlaveMessage> updateSlave1 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(flags.registration_backoff_factor);

  AWAIT_READY(updateSlave1);

  // NOTE: We need to resume the clock so that the resource provider can
  // periodically check if the CSI endpoint socket has been created by
  // the plugin container, which runs in another Linux process.
  Clock::resume();

  AWAIT_READY(updateSlave2);
  ASSERT_TRUE(updateSlave2->has_resource_providers());
  ASSERT_EQ(1, updateSlave2->resource_providers().providers_size());

  Clock::pause();

  // Register a framework to exercise an operation.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;

  auto isRaw = [](
      const Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().type() == Resource::DiskInfo::Source::RAW;
  };

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isRaw, lambda::_1))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers->at(0);

  Option<Resource> source;
  foreach (const Resource& resource, offer.resources()) {
    if (isRaw(resource)) {
      source = resource;
      break;
    }
  }
  ASSERT_SOME(source);

  // We'll drop the first operation status update from the agent to the master.
  Future<UpdateOperationStatusMessage> droppedUpdateOperationStatusMessage =
    DROP_PROTOBUF(
        UpdateOperationStatusMessage(), slave.get()->pid, master.get()->pid);

  // Create a volume.
  driver.acceptOffers(
      {offer.id()},
      {CREATE_VOLUME(source.get(), Resource::DiskInfo::Source::MOUNT)},
      {});

  AWAIT_READY(droppedUpdateOperationStatusMessage);

  // The SLRP should resend the dropped operation status update after the
  // status update retry interval minimum.
  Future<UpdateOperationStatusMessage> retriedUpdateOperationStatusMessage =
    FUTURE_PROTOBUF(
        UpdateOperationStatusMessage(), slave.get()->pid, master.get()->pid);

  // The master should acknowledge the operation status update.
  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatusMessage =
    FUTURE_PROTOBUF(
      AcknowledgeOperationStatusMessage(), master.get()->pid, slave.get()->pid);

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);

  AWAIT_READY(retriedUpdateOperationStatusMessage);
  AWAIT_READY(acknowledgeOperationStatusMessage);

  // The master acknowledged the operation status update, so the SLRP shouldn't
  // send further operation status updates.
  EXPECT_NO_FUTURE_PROTOBUFS(UpdateOperationStatusMessage(), _, _);

  // The master received the `UpdateOperationStatusMessage`, so it can now
  // offer the `MOUNT` disk - no further offers are needed, so they can be
  // declined.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers());

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  driver.stop();
  driver.join();
}


// This test verifies that on agent restarts, unacknowledged operation status
// updates are resent to the master
//
// To accomplish this:
//   1. Creates a volume from a RAW disk resource.
//   2. Drops the first `UpdateOperationStatusMessage` from the agent to the
//      master, so that it isn't acknowledged by the master.
//   3. Restarts the agent.
//   4. Verifies that the agent resends the operation status update.
TEST_F(
    StorageLocalResourceProviderTest,
    ROOT_RetryOperationStatusUpdateAfterRecovery)
{
  Clock::pause();

  loadUriDiskProfileModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileConfig();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux";

  // Disable HTTP authentication to simplify resource provider interactions.
  flags.authenticate_http_readwrite = false;

  // Set the resource provider capability.
  vector<SlaveInfo::Capability> capabilities = slave::AGENT_CAPABILITIES();
  SlaveInfo::Capability capability;
  capability.set_type(SlaveInfo::Capability::RESOURCE_PROVIDER);
  capabilities.push_back(capability);

  flags.agent_features = SlaveCapabilities();
  flags.agent_features->mutable_capabilities()->CopyFrom(
      {capabilities.begin(), capabilities.end()});

  flags.resource_provider_config_dir = resourceProviderConfigDir;
  flags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  //
  // NOTE: The order of the two `FUTURE_PROTOBUF`s are reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlave2 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);
  Future<UpdateSlaveMessage> updateSlave1 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(flags.registration_backoff_factor);

  AWAIT_READY(updateSlave1);

  // NOTE: We need to resume the clock so that the resource provider can
  // periodically check if the CSI endpoint socket has been created by
  // the plugin container, which runs in another Linux process.
  Clock::resume();

  AWAIT_READY(updateSlave2);
  ASSERT_TRUE(updateSlave2->has_resource_providers());
  ASSERT_EQ(1, updateSlave2->resource_providers().providers_size());

  Clock::pause();

  // Register a framework to exercise an operation.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;

  auto isRaw = [](
      const Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().type() == Resource::DiskInfo::Source::RAW;
  };

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isRaw, lambda::_1))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers->at(0);

  Option<Resource> source;
  foreach (const Resource& resource, offer.resources()) {
    if (isRaw(resource)) {
      source = resource;
      break;
    }
  }
  ASSERT_SOME(source);

  // We'll drop the first operation status update from the agent to the master.
  Future<UpdateOperationStatusMessage> droppedUpdateOperationStatusMessage =
    DROP_PROTOBUF(
        UpdateOperationStatusMessage(), slave.get()->pid, master.get()->pid);

  // Create a volume.
  driver.acceptOffers(
      {offer.id()},
      {CREATE_VOLUME(source.get(), Resource::DiskInfo::Source::MOUNT)},
      {});

  AWAIT_READY(droppedUpdateOperationStatusMessage);

  // Restart the agent.
  slave.get()->terminate();

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  Future<UpdateSlaveMessage> updateSlave4 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);
  Future<UpdateSlaveMessage> updateSlave3 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  // Once the agent is restarted, the SLRP should resend the dropped operation
  // status update.
  Future<UpdateOperationStatusMessage> retriedUpdateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, master.get()->pid);

  // The master should acknowledge the operation status update once.
  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatusMessage =
    FUTURE_PROTOBUF(AcknowledgeOperationStatusMessage(), master.get()->pid, _);

  // Decline offers without RAW disk resources, the master can send such offers
  // once it receives the first `UpdateSlaveMessage` after the agent failover,
  // or after receiving the `UpdateOperationStatusMessage`.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers());

  slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(flags.registration_backoff_factor);

  AWAIT_READY(updateSlave3);

  // Resume the clock so that the CSI plugin's standalone container is created
  // and the SLRP's async loop notices it.
  Clock::resume();

  AWAIT_READY(updateSlave4);
  ASSERT_TRUE(updateSlave4->has_resource_providers());
  ASSERT_EQ(1, updateSlave4->resource_providers().providers_size());

  Clock::pause();

  AWAIT_READY(retriedUpdateOperationStatusMessage);

  AWAIT_READY(acknowledgeOperationStatusMessage);

  // The master has acknowledged the operation status update, so the SLRP
  // shouldn't send further operation status updates.
  EXPECT_NO_FUTURE_PROTOBUFS(UpdateOperationStatusMessage(), _, _);

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
