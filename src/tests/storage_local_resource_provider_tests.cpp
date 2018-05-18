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

#include <stout/os/realpath.hpp>

#include "csi/paths.hpp"
#include "csi/state.hpp"

#include "linux/fs.hpp"

#include "master/detector/standalone.hpp"

#include "module/manager.hpp"

#include "slave/container_daemon_process.hpp"
#include "slave/paths.hpp"
#include "slave/state.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using std::list;
using std::string;
using std::vector;

using mesos::internal::slave::ContainerDaemonProcess;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Owned;

using testing::AtMost;
using testing::DoAll;
using testing::Not;
using testing::Sequence;

namespace mesos {
namespace internal {
namespace tests {

constexpr char URI_DISK_PROFILE_ADAPTOR_NAME[] =
  "org_apache_mesos_UriDiskProfileAdaptor";

constexpr char TEST_SLRP_TYPE[] = "org.apache.mesos.rp.local.storage";
constexpr char TEST_SLRP_NAME[] = "test";


class StorageLocalResourceProviderTest
  : public ContainerizerTest<slave::MesosContainerizer>
{
public:
  virtual void SetUp()
  {
    ContainerizerTest<slave::MesosContainerizer>::SetUp();

    resourceProviderConfigDir =
      path::join(sandbox.get(), "resource_provider_configs");
    ASSERT_SOME(os::mkdir(resourceProviderConfigDir));

    uriDiskProfileMappingPath =
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

    foreach (const string& slaveWorkDir, slaveWorkDirs) {
      // Clean up CSI endpoint directories if there is any.
      const string csiRootDir = slave::paths::getCsiRootDir(slaveWorkDir);

      Try<list<string>> csiContainerPaths =
        csi::paths::getContainerPaths(csiRootDir, "*", "*");
      ASSERT_SOME(csiContainerPaths);

      foreach (const string& path, csiContainerPaths.get()) {
        Try<csi::paths::ContainerPath> containerPath =
          csi::paths::parseContainerPath(csiRootDir, path);
        ASSERT_SOME(containerPath);

        Result<string> endpointDir =
          os::realpath(csi::paths::getEndpointDirSymlinkPath(
              csiRootDir,
              containerPath->type,
              containerPath->name,
              containerPath->containerId));

        if (endpointDir.isSome()) {
          ASSERT_SOME(os::rmdir(endpointDir.get()));
        }
      }
    }

    ContainerizerTest<slave::MesosContainerizer>::TearDown();
  }

  virtual slave::Flags CreateSlaveFlags()
  {
    slave::Flags flags =
      ContainerizerTest<slave::MesosContainerizer>::CreateSlaveFlags();

    // Store the agent work directory for cleaning up CSI endpoint
    // directories during teardown.
    // NOTE: DO NOT change the work directory afterward.
    slaveWorkDirs.push_back(flags.work_dir);

    return flags;
  }

  void loadUriDiskProfileAdaptorModule()
  {
    const string libraryPath = getModulePath("uri_disk_profile_adaptor");

    Modules::Library* library = modules.add_libraries();
    library->set_name("uri_disk_profile_adaptor");
    library->set_file(libraryPath);

    Modules::Library::Module* module = library->add_modules();
    module->set_name(URI_DISK_PROFILE_ADAPTOR_NAME);

    Parameter* uri = module->add_parameters();
    uri->set_key("uri");
    uri->set_value(uriDiskProfileMappingPath);
    Parameter* pollInterval = module->add_parameters();
    pollInterval->set_key("poll_interval");
    pollInterval->set_value("1secs");

    ASSERT_SOME(modules::ModuleManager::load(modules));
  }

  void setupResourceProviderConfig(
      const Bytes& capacity,
      const Option<string> volumes = None())
  {
    const string testCsiPluginName = "test_csi_plugin";

    const string testCsiPluginPath =
      path::join(tests::flags.build_dir, "src", "test-csi-plugin");

    const string testCsiPluginWorkDir =
      path::join(sandbox.get(), testCsiPluginName);
    ASSERT_SOME(os::mkdir(testCsiPluginWorkDir));

    Try<string> resourceProviderConfig = strings::format(
        R"~(
        {
          "type": "%s",
          "name": "%s",
          "default_reservations": [
            {
              "type": "DYNAMIC",
              "role": "storage"
            }
          ],
          "storage": {
            "plugin": {
              "type": "org.apache.mesos.csi.test",
              "name": "%s",
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
        TEST_SLRP_TYPE,
        TEST_SLRP_NAME,
        testCsiPluginName,
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

  void setupDiskProfileMapping()
  {
    Try<Nothing> write = os::write(
        uriDiskProfileMappingPath,
        R"~(
        {
          "profile_matrix": {
            "volume-default": {
              "csi_plugin_type_selector": {
                "plugin_type": "org.apache.mesos.csi.test"
              },
              "volume_capabilities": {
                "mount": {},
                "access_mode": {
                  "mode": "SINGLE_NODE_WRITER"
                }
              }
            },
            "block-default": {
              "csi_plugin_type_selector": {
                "plugin_type": "org.apache.mesos.csi.test"
              },
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
  vector<string> slaveWorkDirs;
  string resourceProviderConfigDir;
  string uriDiskProfileMappingPath;
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

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
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
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
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

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
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
  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Kilobytes(512), "volume0:512KB");
  setupDiskProfileMapping();

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> rawDisksOffers;

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that do not contain wanted resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // We expect to receive an offer that contains a storage pool and a
  // pre-existing volume.
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

  // Since the resource provider always reports the pre-existing volume,
  // but only reports the storage pool after it gets the profile, an
  // offer containing the latter will also contain the former.
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

  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
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
  setupDiskProfileMapping();

  // A new storage pool for profile "volume-default" should be reported
  // by the resource provider. Still expect no storage pool for
  // "block-default" since it is not supported by the test CSI plugin.
  AWAIT_READY(updateSlave3);
  ASSERT_TRUE(updateSlave3->has_resource_providers());
  ASSERT_EQ(1, updateSlave3->resource_providers().providers_size());
  EXPECT_EQ(
      1,
      updateSlave3->resource_providers().providers(0).total_resources_size());

  // A storage pool is a RAW disk that has a profile but no ID.
  auto isStoragePool = [](const Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().type() == Resource::DiskInfo::Source::RAW &&
      !r.disk().source().has_id() &&
      r.disk().source().has_profile();
  };

  Option<Resource> volumeStoragePool;
  Option<Resource> blockStoragePool;
  foreach (const Resource& resource,
           updateSlave3->resource_providers().providers(0).total_resources()) {
    if (!isStoragePool(resource)) {
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
  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileMapping();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing a RAW disk resource before `CREATE_VOLUME`.
  //   2. One containing a MOUNT disk resource after `CREATE_VOLUME`.
  //   3. One containing a RAW disk resource after `DESTROY_VOLUME`.
  //
  // We set up the expectations for these offers as the test progresses.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> volumeCreatedOffers;
  Future<vector<Offer>> volumeDestroyedOffers;

  Sequence offers;

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // We are only interested in any storage pool or created volume which
  // has a "volume-default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "volume-default" &&
      r.disk().source().type() == type;
  };

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

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

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
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::RAW))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeDestroyedOffers));

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
  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileMapping();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing a RAW disk resource before `CREATE_VOLUME`.
  //   2. One containing a MOUNT disk resource after `CREATE_VOLUME`.
  //   3. One containing a MOUNT disk resource after the agent recovers
  //      from a failover.
  //   4. One containing a RAW disk resource after `DESTROY_VOLUME`.
  //
  // We set up the expectations for these offers as the test progresses.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> volumeCreatedOffers;
  Future<vector<Offer>> slaveRecoveredOffers;
  Future<vector<Offer>> volumeDestroyedOffers;

  Sequence offers;

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // We are only interested in any storage pool or created volume which
  // has a "volume-default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "volume-default" &&
      r.disk().source().type() == type;
  };

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

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

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
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave.get()->terminate();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::MOUNT))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&slaveRecoveredOffers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRecoveredOffers);
  ASSERT_FALSE(slaveRecoveredOffers->empty());

  // Destroy the created volume.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(hasSourceType, lambda::_1, Resource::DiskInfo::Source::RAW))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeDestroyedOffers));

  driver.acceptOffers(
      {slaveRecoveredOffers->at(0).id()},
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


// This test verifies that if an agent is registered with a new ID,
// the ID of the resource provider would be changed as well, and any
// created volume becomes a pre-existing volume.
TEST_F(StorageLocalResourceProviderTest, ROOT_AgentRegisteredWithNewId)
{
  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileMapping();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing a RAW disk resource before `CREATE_VOLUME`.
  //   2. One containing a MOUNT disk resource after `CREATE_VOLUME`.
  //   3. One containing a RAW pre-existing volume after the agent
  //      is registered with a new ID.
  //
  // We set up the expectations for these offers as the test progresses.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> volumeCreatedOffers;
  Future<vector<Offer>> slaveRecoveredOffers;

  Sequence offers;

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // Before the agent fails over, we are interested in any storage pool
  // or created volume which has a "volume-default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "volume-default" &&
      r.disk().source().type() == type;
  };

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

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

  driver.acceptOffers(
      {rawDiskOffers->at(0).id()},
      {CREATE_VOLUME(source.get(), Resource::DiskInfo::Source::MOUNT)},
      acceptFilters);

  AWAIT_READY(volumeCreatedOffers);
  ASSERT_FALSE(volumeCreatedOffers->empty());

  Option<Resource> createdVolume;

  foreach (const Resource& resource, volumeCreatedOffers->at(0).resources()) {
    if (hasSourceType(resource, Resource::DiskInfo::Source::MOUNT)) {
      createdVolume = resource;
      break;
    }
  }

  ASSERT_SOME(createdVolume);
  ASSERT_TRUE(createdVolume->has_provider_id());
  ASSERT_TRUE(createdVolume->disk().source().has_id());
  ASSERT_TRUE(createdVolume->disk().source().has_metadata());
  ASSERT_TRUE(createdVolume->disk().source().has_mount());
  ASSERT_TRUE(createdVolume->disk().source().mount().has_root());
  EXPECT_FALSE(path::absolute(createdVolume->disk().source().mount().root()));

  // Check if the volume is actually created by the test CSI plugin.
  Option<string> volumePath;

  foreach (const Label& label,
           createdVolume->disk().source().metadata().labels()) {
    if (label.key() == "path") {
      volumePath = label.value();
      break;
    }
  }

  ASSERT_SOME(volumePath);
  EXPECT_TRUE(os::exists(volumePath.get()));

  // Shut down the agent.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave.get()->terminate();

  // Remove the `latest` symlink to register the agent with a new ID.
  const string metaDir = slave::paths::getMetaRootDir(slaveFlags.work_dir);
  ASSERT_SOME(os::rm(slave::paths::getLatestSlavePath(metaDir)));

  // A new registration would trigger another `SlaveRegisteredMessage`.
  slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, Not(slave.get()->pid));

  // After the agent fails over, any volume created before becomes a
  // pre-existing volume, which has an ID but no profile.
  auto isPreExistingVolume = [](const Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_id() &&
      !r.disk().source().has_profile();
  };

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      isPreExistingVolume)))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&slaveRecoveredOffers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  AWAIT_READY(slaveRecoveredOffers);
  ASSERT_FALSE(slaveRecoveredOffers->empty());

  Option<Resource> preExistingVolume;

  foreach (const Resource& resource, slaveRecoveredOffers->at(0).resources()) {
    if (isPreExistingVolume(resource) &&
        resource.disk().source().type() == Resource::DiskInfo::Source::RAW) {
      preExistingVolume = resource;
    }
  }

  ASSERT_SOME(preExistingVolume);
  ASSERT_TRUE(preExistingVolume->has_provider_id());
  ASSERT_NE(createdVolume->provider_id(), preExistingVolume->provider_id());
  ASSERT_EQ(
      createdVolume->disk().source().id(),
      preExistingVolume->disk().source().id());
}


// This test verifies that the storage local resource provider can
// publish a volume required by a task, then destroy the published
// volume after the task finishes.
TEST_F(StorageLocalResourceProviderTest, ROOT_PublishResources)
{
  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileMapping();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing a RAW disk resource before `CREATE_VOLUME`.
  //   2. One containing a MOUNT disk resource after `CREATE_VOLUME`.
  //   3. One containing a persistent volume after `CREATE` and `LAUNCH`.
  //   4. One containing the original RAW disk resource after `DESTROY`
  //      and `DESTROY_VOLUME`.
  //
  // We set up the expectations for these offers as the test progresses.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> volumeCreatedOffers;
  Future<vector<Offer>> taskFinishedOffers;
  Future<vector<Offer>> volumeDestroyedOffers;

  Sequence offers;

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // We are only interested in any storage pool or created volume which
  // has a "volume-default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "volume-default" &&
      r.disk().source().type() == type;
  };

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

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

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
  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileMapping();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing a RAW disk resource before `CREATE_VOLUME`.
  //   2. One containing a MOUNT disk resource after `CREATE_VOLUME`.
  //   3. One containing a persistent volume after `CREATE` and `LAUNCH`.
  //   4. One containing the same persistent volume after the agent
  //      recovers from a failover.
  //   5. One containing the same persistent volume after another `LAUNCH`.
  //   6. One containing the original RAW disk resource after `DESTROY`
  //      and `DESTROY_VOLUME`.
  //
  // We set up the expectations for these offers as the test progresses.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> volumeCreatedOffers;
  Future<vector<Offer>> task1FinishedOffers;
  Future<vector<Offer>> slaveRecoveredOffers;
  Future<vector<Offer>> task2FinishedOffers;
  Future<vector<Offer>> volumeDestroyedOffers;

  Sequence offers;

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // We are only interested in any storage pool or created volume which
  // has a "volume-default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "volume-default" &&
      r.disk().source().type() == type;
  };

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

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

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

  {
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
      .WillOnce(FutureArg<1>(&task1FinishedOffers));

    driver.acceptOffers(
        {volumeCreatedOffers->at(0).id()},
        {CREATE(persistentVolume),
         LAUNCH({createTask(
             volumeCreatedOffers->at(0).slave_id(),
             persistentVolume,
             createCommandInfo("touch " + path::join("volume", "file")))})},
        acceptFilters);

    AWAIT_READY(taskStarting);
    EXPECT_EQ(TASK_STARTING, taskStarting->state());

    AWAIT_READY(taskRunning);
    EXPECT_EQ(TASK_RUNNING, taskRunning->state());

    AWAIT_READY(taskFinished);
    EXPECT_EQ(TASK_FINISHED, taskFinished->state());
  }

  AWAIT_READY(task1FinishedOffers);

  // Restart the agent.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave.get()->terminate();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(
      persistentVolume)))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&slaveRecoveredOffers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRecoveredOffers);
  ASSERT_FALSE(slaveRecoveredOffers->empty());

  // Launch another task to read the file that is created by the
  // previous task on the persistent volume.
  {
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
      .WillOnce(FutureArg<1>(&task2FinishedOffers));

    driver.acceptOffers(
        {slaveRecoveredOffers->at(0).id()},
        {LAUNCH({createTask(
             slaveRecoveredOffers->at(0).slave_id(),
             persistentVolume,
             createCommandInfo("test -f " + path::join("volume", "file")))})},
        acceptFilters);

    AWAIT_READY(taskStarting);
    EXPECT_EQ(TASK_STARTING, taskStarting->state());

    AWAIT_READY(taskRunning);
    EXPECT_EQ(TASK_RUNNING, taskRunning->state());

    AWAIT_READY(taskFinished);
    EXPECT_EQ(TASK_FINISHED, taskFinished->state());
  }

  AWAIT_READY(task2FinishedOffers);

  // Destroy the persistent volume and the CSI volume.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(source.get())))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeDestroyedOffers));

  driver.acceptOffers(
      {task2FinishedOffers->at(0).id()},
      {DESTROY(persistentVolume),
       DESTROY_VOLUME(volume.get())},
      acceptFilters);

  AWAIT_READY(volumeDestroyedOffers);
  ASSERT_FALSE(volumeDestroyedOffers->empty());

  // Check if the volume is actually deleted by the test CSI plugin.
  EXPECT_FALSE(os::exists(volumePath.get()));
}


// This test verifies that the storage local resource provider can
// destroy a published volume after agent reboot.
TEST_F(StorageLocalResourceProviderTest, ROOT_PublishResourcesReboot)
{
  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileMapping();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

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

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing a RAW disk resource before `CREATE_VOLUME`.
  //   2. One containing a MOUNT disk resource after `CREATE_VOLUME`.
  //   3. One containing a persistent volume after `CREATE` and `LAUNCH`.
  //   4. One containing the same persistent volume after the agent
  //      recovers from a failover.
  //   5. One containing the same persistent volume after another `LAUNCH`.
  //   6. One containing the original RAW disk resource after `DESTROY`
  //      and `DESTROY_VOLUME`.
  //
  // We set up the expectations for these offers as the test progresses.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> volumeCreatedOffers;
  Future<vector<Offer>> task1FinishedOffers;
  Future<vector<Offer>> slaveRecoveredOffers;
  Future<vector<Offer>> task2FinishedOffers;
  Future<vector<Offer>> volumeDestroyedOffers;

  Sequence offers;

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // We are only interested in any storage pool or created volume which
  // has a "volume-default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "volume-default" &&
      r.disk().source().type() == type;
  };

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

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

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

  {
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
      .WillOnce(FutureArg<1>(&task1FinishedOffers));

    driver.acceptOffers(
        {volumeCreatedOffers->at(0).id()},
        {CREATE(persistentVolume),
         LAUNCH({createTask(
             volumeCreatedOffers->at(0).slave_id(),
             persistentVolume,
             createCommandInfo("touch " + path::join("volume", "file")))})},
        acceptFilters);

    AWAIT_READY(taskStarting);
    EXPECT_EQ(TASK_STARTING, taskStarting->state());

    AWAIT_READY(taskRunning);
    EXPECT_EQ(TASK_RUNNING, taskRunning->state());

    AWAIT_READY(taskFinished);
    EXPECT_EQ(TASK_FINISHED, taskFinished->state());
  }

  AWAIT_READY(task1FinishedOffers);

  // Destruct the agent to shut down all containers.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave->reset();

  // Modify the boot ID to simulate a reboot.
  ASSERT_SOME(os::write(
      slave::paths::getBootIdPath(
          slave::paths::getMetaRootDir(slaveFlags.work_dir)),
      "rebooted! ;)"));

  const string csiRootDir = slave::paths::getCsiRootDir(slaveFlags.work_dir);

  Try<list<string>> volumePaths =
    csi::paths::getVolumePaths(csiRootDir, "*", "*");
  ASSERT_SOME(volumePaths);
  ASSERT_FALSE(volumePaths->empty());

  foreach (const string& path, volumePaths.get()) {
    Try<csi::paths::VolumePath> volumePath =
      csi::paths::parseVolumePath(csiRootDir, path);
    ASSERT_SOME(volumePath);

    const string volumeStatePath = csi::paths::getVolumeStatePath(
        csiRootDir,
        volumePath->type,
        volumePath->name,
        volumePath->volumeId);

    Result<csi::state::VolumeState> volumeState =
      slave::state::read<csi::state::VolumeState>(volumeStatePath);
    ASSERT_SOME(volumeState);

    if (volumeState->state() == csi::state::VolumeState::PUBLISHED) {
      volumeState->set_boot_id("rebooted! ;)");
      ASSERT_SOME(slave::state::checkpoint(volumeStatePath, volumeState.get()));
    }
  }

  // Unmount all CSI volumes to simulate a reboot.
  ASSERT_SOME(fs::unmountAll(csiRootDir));

  // Restart the agent.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(
      persistentVolume)))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&slaveRecoveredOffers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRecoveredOffers);
  ASSERT_FALSE(slaveRecoveredOffers->empty());

  // Launch another task to read the file that is created by the
  // previous task on the persistent volume.
  {
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
      .WillOnce(FutureArg<1>(&task2FinishedOffers));

    driver.acceptOffers(
        {slaveRecoveredOffers->at(0).id()},
        {LAUNCH({createTask(
             slaveRecoveredOffers->at(0).slave_id(),
             persistentVolume,
             createCommandInfo("test -f " + path::join("volume", "file")))})},
        acceptFilters);

    AWAIT_READY(taskStarting);
    EXPECT_EQ(TASK_STARTING, taskStarting->state());

    AWAIT_READY(taskRunning);
    EXPECT_EQ(TASK_RUNNING, taskRunning->state());

    AWAIT_READY(taskFinished);
    EXPECT_EQ(TASK_FINISHED, taskFinished->state());
  }

  AWAIT_READY(task2FinishedOffers);

  // Destroy the persistent volume and the CSI volume.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(source.get())))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeDestroyedOffers));

  driver.acceptOffers(
      {task2FinishedOffers->at(0).id()},
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
  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileMapping();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(50);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

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

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // We are only interested in any storage pool or created volume which
  // has a "volume-default" profile.
  auto hasSourceType = [](
      const Resource& r,
      const Resource::DiskInfo::Source::Type& type) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().profile() == "volume-default" &&
      r.disk().source().type() == type;
  };

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

  // We use the following filter so that the resources will not be
  // filtered for 5 seconds (the default).
  Filters acceptFilters;
  acceptFilters.set_refuse_seconds(0);

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
  Clock::pause();

  setupResourceProviderConfig(Bytes(0), "volume1:2GB;volume2:2GB");

  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
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

  Clock::pause();

  // Register a framework to exercise operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing two RAW pre-existing volumes before
  //      `CREATE_VOLUME` and `CREATE_BLOCK`.
  //   2. One containing a MOUNT and a BLOCK disk resources after
  //      `CREATE_VOLUME` and `CREATE_BLOCK`.
  //   3. One containing two RAW pre-existing volumes after
  //      `DESTROY_VOLUME` and `DESTROY_BLOCK`.
  //
  // We set up the expectations for these offers as the test progresses.
  Future<vector<Offer>> rawDisksOffers;
  Future<vector<Offer>> disksConvertedOffers;
  Future<vector<Offer>> disksRevertedOffers;

  // We are only interested in any pre-existing volume, which has an ID
  // but no profile.
  auto isPreExistingVolume = [](const Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_id() &&
      !r.disk().source().has_profile();
  };

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      isPreExistingVolume)))
    .WillOnce(FutureArg<1>(&rawDisksOffers));

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
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      isPreExistingVolume)))
    .WillOnce(FutureArg<1>(&disksConvertedOffers));

  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateOperationStatusMessage> createBlockStatusUpdate =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);
  Future<UpdateOperationStatusMessage> createVolumeStatusUpdate =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  driver.acceptOffers(
      {rawDisksOffers->at(0).id()},
      {CREATE_VOLUME(sources.at(0), Resource::DiskInfo::Source::MOUNT),
       CREATE_BLOCK(sources.at(1))});

  AWAIT_READY(createVolumeStatusUpdate);
  AWAIT_READY(createBlockStatusUpdate);

  // Advance the clock to trigger another allocation.
  Clock::advance(masterFlags.allocation_interval);

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
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      isPreExistingVolume)))
    .WillOnce(FutureArg<1>(&disksRevertedOffers));

  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateOperationStatusMessage> destroyBlockStatusUpdate =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);
  Future<UpdateOperationStatusMessage> destroyVolumeStatusUpdate =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  driver.acceptOffers(
      {disksConvertedOffers->at(0).id()},
      {DESTROY_VOLUME(volume.get()),
       DESTROY_BLOCK(block.get())});

  AWAIT_READY(destroyVolumeStatusUpdate);
  AWAIT_READY(destroyBlockStatusUpdate);

  // Advance the clock to trigger another allocation.
  Clock::advance(masterFlags.allocation_interval);

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

  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileMapping();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux";

  flags.resource_provider_config_dir = resourceProviderConfigDir;
  flags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  //
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
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

  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileMapping();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux";

  flags.resource_provider_config_dir = resourceProviderConfigDir;
  flags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  //
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
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


// This test verifies that storage local resource provider metrics are
// properly reported.
TEST_F(StorageLocalResourceProviderTest, ROOT_Metrics)
{
  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  slave::Fetcher fetcher(slaveFlags);

  Try<slave::MesosContainerizer*> _containerizer =
    slave::MesosContainerizer::create(slaveFlags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::MesosContainerizer> containerizer(_containerizer.get());

  Future<Nothing> pluginConnected =
    FUTURE_DISPATCH(_, &ContainerDaemonProcess::waitContainer);

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      slaveFlags);

  ASSERT_SOME(slave);

  AWAIT_READY(pluginConnected);

  const string prefix =
    "resource_providers/" + stringify(TEST_SLRP_TYPE) +
    "." + stringify(TEST_SLRP_NAME) + "/";

  JSON::Object snapshot = Metrics();

  ASSERT_NE(0u, snapshot.values.count(
      prefix + "csi_controller_plugin_terminations"));
  EXPECT_EQ(0, snapshot.values.at(
      prefix + "csi_controller_plugin_terminations"));
  ASSERT_NE(0u, snapshot.values.count(
      prefix + "csi_node_plugin_terminations"));
  EXPECT_EQ(0, snapshot.values.at(
      prefix + "csi_node_plugin_terminations"));

  // Get the ID of the CSI plugin container.
  Future<hashset<ContainerID>> pluginContainers = containerizer->containers();

  AWAIT_READY(pluginContainers);
  ASSERT_EQ(1u, pluginContainers->size());

  const ContainerID& pluginContainerId = *pluginContainers->begin();

  Future<Nothing> pluginRestarted =
    FUTURE_DISPATCH(_, &ContainerDaemonProcess::launchContainer);

  // Kill the plugin container and wait for it to restart.
  // NOTE: We need to wait for `pluginConnected` before issuing the
  // kill, or it may kill the plugin before it created the endpoint
  // socket and the resource provider would wait for one minute.
  Future<int> pluginKilled = containerizer->status(pluginContainerId)
    .then([](const ContainerStatus& status) {
      return os::kill(status.executor_pid(), SIGKILL);
    });

  AWAIT_ASSERT_EQ(0, pluginKilled);
  AWAIT_READY(pluginRestarted);

  snapshot = Metrics();

  ASSERT_NE(0u, snapshot.values.count(
      prefix + "csi_controller_plugin_terminations"));
  EXPECT_EQ(1, snapshot.values.at(
      prefix + "csi_controller_plugin_terminations"));
  ASSERT_NE(0u, snapshot.values.count(
      prefix + "csi_node_plugin_terminations"));
  EXPECT_EQ(1, snapshot.values.at(
      prefix + "csi_node_plugin_terminations"));
}


// Master reconciles operations that are missing from a reregistering slave.
// In this case, the `ApplyOperationMessage` is dropped, so the resource
// provider should send OPERATION_DROPPED. Operations on agent default
// resources are also tested here; for such operations, the agent generates the
// dropped status.
TEST_F(StorageLocalResourceProviderTest, ROOT_ReconcileDroppedOperation)
{
  Clock::pause();

  setupResourceProviderConfig(Bytes(0), "volume1:2GB;volume2:2GB");

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

  slaveFlags.resource_provider_config_dir = resourceProviderConfigDir;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Since the local resource provider daemon is started after the agent is
  // registered, it is guaranteed that the agent will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from the
  // storage local resource provider.
  //
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlave2 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);
  Future<UpdateSlaveMessage> updateSlave1 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);

  // NOTE: We need to resume the clock so that the resource provider can
  // periodically check if the CSI endpoint socket has been created by the
  // plugin container, which runs in another Linux process. Since we do not have
  // a `Future` linked to the standalone container launch to await on, it is
  // difficult to accomplish this without resuming the clock.
  Clock::resume();

  AWAIT_READY(updateSlave2);
  ASSERT_TRUE(updateSlave2->has_resource_providers());
  ASSERT_EQ(1, updateSlave2->resource_providers().providers_size());

  Clock::pause();

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // We are only interested in pre-existing volumes, which have IDs but no
  // profile. We use pre-existing volumes to make it easy to send multiple
  // operations on multiple resources.
  auto isPreExistingVolume = [](const Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_id() &&
      !r.disk().source().has_profile();
  };

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(filters));

  Future<vector<Offer>> offersBeforeOperations;

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      isPreExistingVolume)))
    .WillOnce(FutureArg<1>(&offersBeforeOperations))
    .WillRepeatedly(DeclineOffers(filters)); // Decline further matching offers.

  driver.start();

  AWAIT_READY(offersBeforeOperations);
  ASSERT_FALSE(offersBeforeOperations->empty());

  vector<Resource> sources;

  foreach (
      const Resource& resource,
      offersBeforeOperations->at(0).resources()) {
    if (isPreExistingVolume(resource) &&
        resource.disk().source().type() == Resource::DiskInfo::Source::RAW) {
      sources.push_back(resource);
    }
  }

  ASSERT_EQ(2u, sources.size());

  // Drop one of the operations on the way to the agent.
  Future<ApplyOperationMessage> applyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), _, _);

  // The successful operation will result in a terminal update.
  Future<UpdateOperationStatusMessage> operationFinishedStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  // Attempt the creation of two volumes.
  driver.acceptOffers(
      {offersBeforeOperations->at(0).id()},
      {CREATE_VOLUME(sources.at(0), Resource::DiskInfo::Source::MOUNT),
       CREATE_VOLUME(sources.at(1), Resource::DiskInfo::Source::MOUNT)},
      filters);

  // Ensure that the operations are processed.
  Clock::settle();

  AWAIT_READY(applyOperationMessage);
  AWAIT_READY(operationFinishedStatus);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Observe explicit operation reconciliation between master and agent.
  Future<ReconcileOperationsMessage> reconcileOperationsMessage =
    FUTURE_PROTOBUF(ReconcileOperationsMessage(), _, _);
  Future<UpdateOperationStatusMessage> operationDroppedStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  // The master may send an offer with the agent's resources after the agent
  // reregisters, but before an `UpdateSlaveMessage` is sent containing the
  // resource provider's resources. In this case, the offer will be rescinded.
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  // Simulate a spurious master change event (e.g., due to ZooKeeper
  // expiration) at the slave to force re-registration.
  detector.appoint(master.get()->pid);

  // Advance the clock to trigger agent registration.
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveReregisteredMessage);
  AWAIT_READY(reconcileOperationsMessage);
  AWAIT_READY(operationDroppedStatus);

  std::set<OperationState> expectedStates =
    {OperationState::OPERATION_DROPPED,
     OperationState::OPERATION_FINISHED};

  std::set<OperationState> observedStates =
    {operationFinishedStatus->status().state(),
     operationDroppedStatus->status().state()};

  ASSERT_EQ(expectedStates, observedStates);

  Future<vector<Offer>> offersAfterOperations;

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      isPreExistingVolume)))
    .WillOnce(FutureArg<1>(&offersAfterOperations));

  // Advance the clock to trigger a batch allocation.
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterOperations);
  ASSERT_FALSE(offersAfterOperations->empty());

  vector<Resource> converted;

  foreach (const Resource& resource, offersAfterOperations->at(0).resources()) {
    if (isPreExistingVolume(resource) &&
        resource.disk().source().type() == Resource::DiskInfo::Source::MOUNT) {
      converted.push_back(resource);
    }
  }

  ASSERT_EQ(1u, converted.size());

  // TODO(greggomann): Add inspection of dropped operation metrics here once
  // such metrics have been added. See MESOS-8406.

  // Settle the clock to ensure that unexpected messages will cause errors.
  Clock::settle();

  driver.stop();
  driver.join();
}


// This test verifies that if an operation ID is specified, operation status
// updates are resent to the scheduler until acknowledged.
TEST_F(
    StorageLocalResourceProviderTest,
    ROOT_RetryOperationStatusUpdateToScheduler)
{
  Clock::pause();

  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileMapping();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux";

  flags.resource_provider_config_dir = resourceProviderConfigDir;
  flags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  //
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
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
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "storage");

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  // Decline offers that do not contain wanted resources.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(v1::scheduler::DeclineOffers());

  Future<v1::scheduler::Event::Offers> offers;

  auto isRaw = [](const v1::Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().type() == v1::Resource::DiskInfo::Source::RAW;
  };

  EXPECT_CALL(*scheduler, offers(_, v1::scheduler::OffersHaveAnyResource(
      std::bind(isRaw, lambda::_1))))
    .WillOnce(FutureArg<1>(&offers));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  // NOTE: If the framework has not declined an unwanted offer yet when
  // the master updates the agent with the RAW disk resource, the new
  // allocation triggered by this update won't generate an allocatable
  // offer due to no CPU and memory resources. So here we first settle
  // the clock to ensure that the unwanted offer has been declined, then
  // advance the clock to trigger another allocation.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  Option<v1::Resource> source;
  Option<mesos::v1::ResourceProviderID> resourceProviderId;
  foreach (const v1::Resource& resource, offer.resources()) {
    if (isRaw(resource)) {
      source = resource;

      ASSERT_TRUE(resource.has_provider_id());
      resourceProviderId = resource.provider_id();

      break;
    }
  }

  ASSERT_SOME(source);
  ASSERT_SOME(resourceProviderId);

  Future<v1::scheduler::Event::UpdateOperationStatus> update;
  Future<v1::scheduler::Event::UpdateOperationStatus> retriedUpdate;

  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&update))
    .WillOnce(FutureArg<1>(&retriedUpdate));

  // Create a volume.
  const string operationId = "operation";
  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {v1::CREATE_VOLUME(
          source.get(), v1::Resource::DiskInfo::Source::MOUNT, operationId)}));

  AWAIT_READY(update);

  ASSERT_EQ(operationId, update->status().operation_id().value());
  ASSERT_EQ(
      mesos::v1::OperationState::OPERATION_FINISHED, update->status().state());
  ASSERT_TRUE(update->status().has_uuid());

  ASSERT_TRUE(retriedUpdate.isPending());

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  // The scheduler didn't acknowledge the operation status update, so the SLRP
  // should resend it after the status update retry interval minimum.
  AWAIT_READY(retriedUpdate);

  ASSERT_EQ(operationId, retriedUpdate->status().operation_id().value());
  ASSERT_EQ(
      mesos::v1::OperationState::OPERATION_FINISHED,
      retriedUpdate->status().state());
  ASSERT_TRUE(retriedUpdate->status().has_uuid());

  // The scheduler will acknowledge the operation status update, so the agent
  // should receive an acknowledgement.
  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatusMessage =
    FUTURE_PROTOBUF(
      AcknowledgeOperationStatusMessage(), master.get()->pid, slave.get()->pid);

  mesos.send(v1::createCallAcknowledgeOperationStatus(
      frameworkId, offer.agent_id(), resourceProviderId.get(), update.get()));

  AWAIT_READY(acknowledgeOperationStatusMessage);

  // Now that the SLRP has received the acknowledgement, the SLRP shouldn't
  // send further operation status updates.
  EXPECT_NO_FUTURE_PROTOBUFS(UpdateOperationStatusMessage(), _, _);

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();
}


// This test ensures that the master responds with the latest state
// for operations that are terminal at the master, but have not been
// acknowledged by the framework.
TEST_F(
    StorageLocalResourceProviderTest,
    ROOT_ReconcileUnacknowledgedTerminalOperation)
{
  Clock::pause();

  loadUriDiskProfileAdaptorModule();

  setupResourceProviderConfig(Gigabytes(4));
  setupDiskProfileMapping();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux";

  flags.resource_provider_config_dir = resourceProviderConfigDir;
  flags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider daemon is started after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains resources from
  // the storage local resource provider.
  //
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
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
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "storage");

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  // Decline offers that do not contain wanted resources.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(v1::scheduler::DeclineOffers());

  Future<v1::scheduler::Event::Offers> offers;

  auto isRaw = [](const v1::Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().type() == v1::Resource::DiskInfo::Source::RAW;
  };

  EXPECT_CALL(*scheduler, offers(_, v1::scheduler::OffersHaveAnyResource(
      std::bind(isRaw, lambda::_1))))
    .WillOnce(FutureArg<1>(&offers));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  // NOTE: If the framework has not declined an unwanted offer yet when
  // the master updates the agent with the RAW disk resource, the new
  // allocation triggered by this update won't generate an allocatable
  // offer due to no CPU and memory resources. So here we first settle
  // the clock to ensure that the unwanted offer has been declined, then
  // advance the clock to trigger another allocation.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  Future<v1::scheduler::Event::UpdateOperationStatus> update;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&update));

  Option<v1::Resource> source;
  Option<mesos::v1::ResourceProviderID> resourceProviderId;
  foreach (const v1::Resource& resource, offer.resources()) {
    if (isRaw(resource)) {
      source = resource;

      ASSERT_TRUE(resource.has_provider_id());
      resourceProviderId = resource.provider_id();

      break;
    }
  }

  ASSERT_SOME(source);
  ASSERT_SOME(resourceProviderId);

  v1::OperationID operationId;
  operationId.set_value("operation");

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {v1::CREATE_VOLUME(
          source.get(),
          v1::Resource::DiskInfo::Source::MOUNT,
          operationId.value())}));

  AWAIT_READY(update);

  ASSERT_EQ(operationId, update->status().operation_id());
  ASSERT_EQ(v1::OperationState::OPERATION_FINISHED, update->status().state());
  ASSERT_TRUE(update->status().has_uuid());

  v1::scheduler::Call::ReconcileOperations::Operation operation;
  operation.mutable_operation_id()->CopyFrom(operationId);
  operation.mutable_agent_id()->CopyFrom(agentId);

  const Future<v1::scheduler::APIResult> result =
    mesos.call({v1::createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '200 OK' and with a `scheduler::Response`.
  ASSERT_EQ(process::http::Status::OK, result->status_code());
  ASSERT_TRUE(result->has_response());

  const v1::scheduler::Response response = result->response();
  ASSERT_EQ(v1::scheduler::Response::RECONCILE_OPERATIONS, response.type());
  ASSERT_TRUE(response.has_reconcile_operations());

  const v1::scheduler::Response::ReconcileOperations& reconcile =
    response.reconcile_operations();
  ASSERT_EQ(1, reconcile.operation_statuses_size());

  const v1::OperationStatus& operationStatus = reconcile.operation_statuses(0);
  ASSERT_EQ(operationId, operationStatus.operation_id());
  ASSERT_EQ(v1::OPERATION_FINISHED, operationStatus.state());
  ASSERT_TRUE(operationStatus.has_uuid());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
