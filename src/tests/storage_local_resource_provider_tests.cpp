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

#include <algorithm>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <google/protobuf/repeated_field.h>

#include <mesos/csi/v0.hpp>
#include <mesos/csi/v1.hpp>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/grpc.hpp>
#include <process/gtest.hpp>
#include <process/gmock.hpp>
#include <process/protobuf.hpp>
#include <process/queue.hpp>
#include <process/reap.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/unreachable.hpp>
#include <stout/uri.hpp>
#include <stout/uuid.hpp>

#include <stout/os/realpath.hpp>

#include "csi/constants.hpp"
#include "csi/paths.hpp"
#include "csi/state.hpp"
#include "csi/v0_volume_manager_process.hpp"
#include "csi/v1_volume_manager_process.hpp"
#include "csi/volume_manager.hpp"

#include "linux/fs.hpp"

#include "master/detector/standalone.hpp"

#include "module/manager.hpp"

#include "messages/messages.hpp"

#include "resource_provider/constants.hpp"

#include "slave/container_daemon_process.hpp"
#include "slave/paths.hpp"
#include "slave/state.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "status_update_manager/status_update_manager_process.hpp"

#include "tests/disk_profile_server.hpp"
#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/mock_csi_plugin.hpp"

namespace http = process::http;

using std::list;
using std::multiset;
using std::shared_ptr;
using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

using mesos::internal::slave::ContainerDaemonProcess;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Owned;
using process::Promise;
using process::post;
using process::Queue;
using process::reap;

using process::grpc::StatusError;

using testing::_;
using testing::A;
using testing::AllOf;
using testing::AtMost;
using testing::Between;
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
constexpr char TEST_CSI_PLUGIN_TYPE[] = "org.apache.mesos.csi.test";
constexpr char TEST_CSI_PLUGIN_NAME[] = "local";
constexpr char TEST_CSI_VENDOR[] = "org.apache.mesos.csi.test.local";


class StorageLocalResourceProviderTest
  : public ContainerizerTest<slave::MesosContainerizer>,
    public testing::WithParamInterface<string>
{
public:
  void SetUp() override
  {
    ContainerizerTest<slave::MesosContainerizer>::SetUp();

    resourceProviderConfigDir =
      path::join(sandbox.get(), "resource_provider_configs");

    ASSERT_SOME(os::mkdir(resourceProviderConfigDir.get()));

    Try<string> mkdtemp = environment->mkdtemp();
    ASSERT_SOME(mkdtemp);

    testCsiPluginWorkDir = mkdtemp.get();
  }

  void TearDown() override
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

  master::Flags CreateMasterFlags() override
  {
    master::Flags flags =
      ContainerizerTest<slave::MesosContainerizer>::CreateMasterFlags();

    // Use a small allocation interval to speed up the test. We do this instead
    // of manipulating the clock because the storage local resource provider
    // relies on a running clock to wait for the CSI plugin to be ready.
    flags.allocation_interval = Milliseconds(50);

    return flags;
  }

  slave::Flags CreateSlaveFlags() override
  {
    slave::Flags flags =
      ContainerizerTest<slave::MesosContainerizer>::CreateSlaveFlags();

    flags.resource_provider_config_dir = resourceProviderConfigDir;

    // Store the agent work directory for cleaning up CSI endpoint
    // directories during teardown.
    // NOTE: DO NOT change the work directory afterward.
    slaveWorkDirs.push_back(flags.work_dir);

    return flags;
  }

  void loadUriDiskProfileAdaptorModule(
      const string& uri,
      const Option<Duration> pollInterval = None())
  {
    const string libraryPath = getModulePath("uri_disk_profile_adaptor");

    Modules::Library* library = modules.add_libraries();
    library->set_name("uri_disk_profile_adaptor");
    library->set_file(libraryPath);

    Modules::Library::Module* module = library->add_modules();
    module->set_name(URI_DISK_PROFILE_ADAPTOR_NAME);

    Parameter* _uri = module->add_parameters();
    _uri->set_key("uri");
    _uri->set_value(uri);

    if (pollInterval.isSome()) {
      Parameter* _pollInterval = module->add_parameters();
      _pollInterval->set_key("poll_interval");
      _pollInterval->set_value(stringify(pollInterval.get()));
    }

    ASSERT_SOME(modules::ModuleManager::load(modules));
  }

  void setupResourceProviderConfig(
      const Bytes& capacity,
      const Option<string>& volumes = None(),
      const Option<string>& forward = None(),
      const Option<string>& createParameters = None(),
      const Option<string>& volumeMetadata = None(),
      const Option<Duration>& reconciliationInterval = None())
  {
    const string testCsiPluginPath =
      path::join(getTestHelperDir(), "test-csi-plugin");

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
              "type": "%s",
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
                      "--api_version=%s",
                      "--work_dir=%s",
                      "--available_capacity=%s",
                      "--volumes=%s",
                      "%s",
                      "--create_parameters=%s",
                      "--volume_metadata=%s"
                    ]
                  },
                  "resources": [
                    {
                      "name": "cpus",
                      "type": "SCALAR",
                      "scalar": {
                        "value": 0.1
                      }
                    },
                    {
                      "name": "mem",
                      "type": "SCALAR",
                      "scalar": {
                        "value": 1024
                      }
                    }
                  ]
                }
              ]
            },
            "reconciliation_interval_seconds" : %s
          }
        }
        )~",
        TEST_SLRP_TYPE,
        TEST_SLRP_NAME,
        TEST_CSI_PLUGIN_TYPE,
        TEST_CSI_PLUGIN_NAME,
        testCsiPluginPath,
        testCsiPluginPath,
        GetParam(),
        testCsiPluginWorkDir.get(),
        stringify(capacity),
        volumes.getOrElse(""),
        forward.isSome() ? "--forward=" + forward.get() : "",
        createParameters.getOrElse(""),
        volumeMetadata.getOrElse(""),
        stringify(reconciliationInterval.getOrElse(Seconds(0)).secs()));

    ASSERT_SOME(resourceProviderConfig);

    ASSERT_SOME(os::write(
        path::join(resourceProviderConfigDir.get(), "test.json"),
        resourceProviderConfig.get()));
  }

  // Set up an expected `CreateVolume` CSI call for a given mock CSI plugin.
  // When the call is made to the mock plugin, `result` will be responded. When
  // the response is received by the volume manager, the returned future will be
  // satisfied.
  Future<Nothing> futureCreateVolumeCall(
      MockCSIPlugin* plugin, const Try<csi::VolumeInfo, StatusError>& result)
  {
    if (GetParam() == csi::v0::API_VERSION) {
      EXPECT_CALL(*plugin, CreateVolume(
          _, _, A<csi::v0::CreateVolumeResponse*>()))
        .WillOnce(Invoke([result](
            grpc::ServerContext* context,
            const csi::v0::CreateVolumeRequest* request,
            csi::v0::CreateVolumeResponse* response) {
          if (result.isError()) {
            return result.error().status;
          }

          response->mutable_volume()->set_id(result->id);
          response->mutable_volume()
            ->set_capacity_bytes(result->capacity.bytes());
          *response->mutable_volume()->mutable_attributes() = result->context;

          return grpc::Status::OK;
        }));

      return FUTURE_DISPATCH(_, &csi::v0::VolumeManagerProcess::__call<
          csi::v0::CreateVolumeResponse>);
    } else if (GetParam() == csi::v1::API_VERSION) {
      EXPECT_CALL(*plugin, CreateVolume(
          _, _, A<csi::v1::CreateVolumeResponse*>()))
        .WillOnce(Invoke([result](
            grpc::ServerContext* context,
            const csi::v1::CreateVolumeRequest* request,
            csi::v1::CreateVolumeResponse* response) {
          if (result.isError()) {
            return result.error().status;
          }

          response->mutable_volume()->set_volume_id(result->id);
          response->mutable_volume()
            ->set_capacity_bytes(result->capacity.bytes());
          *response->mutable_volume()
            ->mutable_volume_context() = result->context;

          return grpc::Status::OK;
        }));

      return FUTURE_DISPATCH(_, &csi::v1::VolumeManagerProcess::__call<
          csi::v1::CreateVolumeResponse>);
    }

    // This extra closure is necessary in order to use `FAIL` as it requires a
    // void return type.
    [&] { FAIL() << "Unsupported CSI API version " << GetParam(); }();

    UNREACHABLE();
  }

  // Set up an expected `DeleteVolume` CSI call for a given mock CSI plugin.
  // When the call is made to the mock plugin, `result` will be responded. When
  // the response is received by the volume manager, the returned future will be
  // satisfied.
  Future<Nothing> futureDeleteVolumeCall(
      MockCSIPlugin* plugin, const Try<Nothing, StatusError>& result)
  {
    if (GetParam() == csi::v0::API_VERSION) {
      EXPECT_CALL(*plugin, DeleteVolume(
          _, _, A<csi::v0::DeleteVolumeResponse*>()))
        .WillOnce(Invoke([result](
            grpc::ServerContext* context,
            const csi::v0::DeleteVolumeRequest* request,
            csi::v0::DeleteVolumeResponse* response) {
          return result.isError() ? result.error().status : grpc::Status::OK;
        }));

      return FUTURE_DISPATCH(_, &csi::v0::VolumeManagerProcess::__call<
          csi::v0::DeleteVolumeResponse>);
    } else if (GetParam() == csi::v1::API_VERSION) {
      EXPECT_CALL(*plugin, DeleteVolume(
          _, _, A<csi::v1::DeleteVolumeResponse*>()))
        .WillOnce(Invoke([result](
            grpc::ServerContext* context,
            const csi::v1::DeleteVolumeRequest* request,
            csi::v1::DeleteVolumeResponse* response) {
          return result.isError() ? result.error().status : grpc::Status::OK;
        }));

      return FUTURE_DISPATCH(_, &csi::v1::VolumeManagerProcess::__call<
          csi::v1::DeleteVolumeResponse>);
    }

    // This extra closure is necessary in order to use `FAIL` as it requires a
    // void return type.
    [&] { FAIL() << "Unsupported CSI API version " << GetParam(); }();

    UNREACHABLE();
  }

  // Set up an expected `NodePublishVolume` CSI call for a given mock CSI
  // plugin. When the call is made to the mock plugin, `result` will be
  // responded. When the response is received by the volume manager, the
  // returned future will be satisfied.
  Future<Nothing> futureNodePublishVolumeCall(
      MockCSIPlugin* plugin, const Try<Nothing, StatusError>& result)
  {
    if (GetParam() == csi::v0::API_VERSION) {
      EXPECT_CALL(*plugin, NodePublishVolume(
          _, _, A<csi::v0::NodePublishVolumeResponse*>()))
        .WillOnce(Invoke([result](
            grpc::ServerContext* context,
            const csi::v0::NodePublishVolumeRequest* request,
            csi::v0::NodePublishVolumeResponse* response) {
          return result.isError() ? result.error().status : grpc::Status::OK;
        }));

      return FUTURE_DISPATCH(_, &csi::v0::VolumeManagerProcess::__call<
          csi::v0::NodePublishVolumeResponse>);
    } else if (GetParam() == csi::v1::API_VERSION) {
      EXPECT_CALL(*plugin, NodePublishVolume(
          _, _, A<csi::v1::NodePublishVolumeResponse*>()))
        .WillOnce(Invoke([result](
            grpc::ServerContext* context,
            const csi::v1::NodePublishVolumeRequest* request,
            csi::v1::NodePublishVolumeResponse* response) {
          return result.isError() ? result.error().status : grpc::Status::OK;
        }));

      return FUTURE_DISPATCH(_, &csi::v1::VolumeManagerProcess::__call<
          csi::v1::NodePublishVolumeResponse>);
    }

    // This extra closure is necessary in order to use `FAIL` as it requires a
    // void return type.
    [&] { FAIL() << "Unsupported CSI API version " << GetParam(); }();

    UNREACHABLE();
  }

  // Create a JSON string representing a disk profile mapping containing the
  // given profile-parameter pairs.
  static string createDiskProfileMapping(
      const hashmap<string, Option<JSON::Object>>& profiles)
  {
    JSON::Object diskProfileMapping{
      {"profile_matrix", JSON::Object{}}
    };

    foreachpair (const string& profile,
                 const Option<JSON::Object>& parameters,
                 profiles) {
      JSON::Object profileInfo{
        {"csi_plugin_type_selector", JSON::Object{
          {"plugin_type", "org.apache.mesos.csi.test"}
        }},
        {"volume_capabilities", JSON::Object{
          {"mount", JSON::Object{}},
          {"access_mode", JSON::Object{
            {"mode", "SINGLE_NODE_WRITER"}
          }}
        }}};

      diskProfileMapping
        .values.at("profile_matrix").as<JSON::Object>()
        .values.emplace(profile, profileInfo);

      if (parameters.isSome()) {
        diskProfileMapping
          .values.at("profile_matrix").as<JSON::Object>()
          .values.at(profile).as<JSON::Object>()
          .values.emplace("create_parameters", parameters.get());
      }
    }

    return stringify(diskProfileMapping);
  }

  static Resources createTaskResources(const Resources& additional)
  {
    return Resources::parse("cpus:0.1;mem:128").get() + additional;
  }

  static string metricName(const string& basename)
  {
    return "resource_providers/" + stringify(TEST_SLRP_TYPE) + "." +
      stringify(TEST_SLRP_NAME) + "/" + basename;
  }

private:
  Modules modules;
  vector<string> slaveWorkDirs;
  Option<string> resourceProviderConfigDir;
  Option<string> testCsiPluginWorkDir;
};


// Tests whether a resource is a storage pool of a given profile. A storage pool
// is a RAW disk resource with a profile but no source ID.
template <typename Resource>
static bool isStoragePool(const Resource& r, const string& profile)
{
  return r.has_disk() &&
    r.disk().has_source() &&
    r.disk().source().type() == Resource::DiskInfo::Source::RAW &&
    r.disk().source().has_vendor() &&
    r.disk().source().vendor() == TEST_CSI_VENDOR &&
    !r.disk().source().has_id() &&
    r.disk().source().has_profile() &&
    r.disk().source().profile() == profile;
}


// Tests whether a resource is a MOUNT disk of a given profile but not a
// persistent volume. A MOUNT disk has both profile and source ID set.
template <typename Resource>
static bool isMountDisk(const Resource& r, const string& profile)
{
  return r.has_disk() &&
    r.disk().has_source() &&
    r.disk().source().type() == Resource::DiskInfo::Source::MOUNT &&
    r.disk().source().has_vendor() &&
    r.disk().source().vendor() == TEST_CSI_VENDOR &&
    r.disk().source().has_id() &&
    r.disk().source().has_profile() &&
    r.disk().source().profile() == profile &&
    !r.disk().has_persistence();
}


// Tests whether a resource is a BLOCK disk of a given profile but not a
// persistent volume. A BLOCK disk has both profile and source ID set.
template <typename Resource>
static bool isBlockDisk(const Resource& r, const string& profile)
{
  return r.has_disk() &&
    r.disk().has_source() &&
    r.disk().source().type() == Resource::DiskInfo::Source::BLOCK &&
    r.disk().source().has_vendor() &&
    r.disk().source().vendor() == TEST_CSI_VENDOR &&
    r.disk().source().has_id() &&
    r.disk().source().has_profile() &&
    r.disk().source().profile() == profile &&
    !r.disk().has_persistence();
}


// Tests whether a resource is a preprovisioned volume. A preprovisioned volume
// is a RAW disk resource with a source ID but no profile.
template <typename Resource>
static bool isPreprovisionedVolume(const Resource& r)
{
  return r.has_disk() &&
    r.disk().has_source() &&
    r.disk().source().type() == Resource::DiskInfo::Source::RAW &&
    r.disk().source().has_vendor() &&
    r.disk().source().vendor() == TEST_CSI_VENDOR &&
    r.disk().source().has_id() &&
    !r.disk().source().has_profile();
}


INSTANTIATE_TEST_CASE_P(
    CSIVersion,
    StorageLocalResourceProviderTest,
    testing::Values(csi::v0::API_VERSION, csi::v1::API_VERSION),
    [](const testing::TestParamInfo<string>& info) { return info.param; });


// This test verifies that a storage local resource provider can report
// no resource and recover from this state.
TEST_P(StorageLocalResourceProviderTest, NoResource)
{
  Clock::pause();

  setupResourceProviderConfig(Bytes(0));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  // Since the local resource provider gets subscribed after the agent
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

  // Since the local resource provider gets subscribed after the agent
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
TEST_P(StorageLocalResourceProviderTest, DISABLED_ZeroSizedDisk)
{
  Clock::pause();

  setupResourceProviderConfig(Bytes(0), "volume0:0B");

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  // Since the local resource provider gets subscribed after the agent
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
TEST_P(StorageLocalResourceProviderTest, DISABLED_SmallDisk)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Kilobytes(512), "volume0:512KB");

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
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

  // Since the resource provider always reports the pre-existing volume,
  // but only reports the storage pool after it gets the profile, an
  // offer containing the latter will also contain the former.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&rawDisksOffers));

  driver.start();

  AWAIT_READY(rawDisksOffers);
  ASSERT_FALSE(rawDisksOffers->empty());

  Option<Resource> storagePool;
  Option<Resource> preExistingVolume;
  foreach (const Resource& resource, rawDisksOffers->at(0).resources()) {
    if (isStoragePool(resource, "test")) {
      storagePool = resource;
    } else if (isPreprovisionedVolume(resource)) {
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


// This test verifies that a framework can receive offers having new storage
// pools from the storage local resource provider after a profile appears.
TEST_P(StorageLocalResourceProviderTest, ProfileAppeared)
{
  Clock::pause();

  Future<shared_ptr<TestDiskProfileServer>> server =
    TestDiskProfileServer::create();
  AWAIT_READY(server);

  Promise<http::Response> updatedProfileMapping;
  EXPECT_CALL(*server.get()->process, profiles(_))
    .WillOnce(Return(http::OK("{}")))
    .WillOnce(Return(updatedProfileMapping.future()));

  const Duration pollInterval = Seconds(10);
  loadUriDiskProfileAdaptorModule(
      stringify(server.get()->process->url()),
      pollInterval);

  setupResourceProviderConfig(Gigabytes(4));

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider gets subscribed after the agent
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

  // Register a framework to receive offers.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  Future<vector<Offer>> offersBeforeProfileFound;
  Future<vector<Offer>> offersAfterProfileFound;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(
        FutureArg<1>(&offersBeforeProfileFound),
        DeclineOffers(declineFilters)))
    .WillOnce(FutureArg<1>(&offersAfterProfileFound));

  driver.start();

  AWAIT_READY(offersBeforeProfileFound);
  ASSERT_FALSE(offersBeforeProfileFound->empty());

  // The offer should not have any resource from the resource provider before
  // the profile appears.
  Resources resourceProviderResources =
    Resources(offersBeforeProfileFound->at(0).resources())
    .filter(&Resources::hasResourceProvider);

  EXPECT_TRUE(resourceProviderResources.empty());

  // Trigger another poll for profiles. The framework will not receive an offer
  // because there is no change in resources yet.
  Clock::advance(pollInterval);
  Clock::settle();

  // Update the disk profile mapping.
  updatedProfileMapping.set(
      http::OK(createDiskProfileMapping({{"test", None()}})));

  // Advance the clock to make sure another allocation is triggered.
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterProfileFound);
  ASSERT_FALSE(offersAfterProfileFound->empty());

  // A new storage pool with profile "test" should show up now.
  Resources storagePools =
    Resources(offersAfterProfileFound->at(0).resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"));

  EXPECT_FALSE(storagePools.empty());
}


// This test verifies that the storage local resource provider can create then
// destroy a MOUNT disk from a storage pool with other pipelined operations.
TEST_P(StorageLocalResourceProviderTest, CreateDestroyDisk)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to exercise operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage/role");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  // Create a MOUNT disk with a pipelined `RESERVE` operation.
  Resource raw = *Resources(offer.resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .begin();

  Resource reserved = *Resources(raw)
    .pushReservation(createDynamicReservationInfo(
        framework.roles(0), framework.principal()))
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {RESERVE(reserved),
       CREATE_DISK(reserved, Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  Resource created = *Resources(offer.resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"))
    .begin();

  ASSERT_TRUE(created.disk().source().has_mount());
  ASSERT_TRUE(created.disk().source().mount().has_root());
  EXPECT_FALSE(path::is_absolute(created.disk().source().mount().root()));

  // Check if the volume is actually created by the test CSI plugin.
  const string& volumePath = created.disk().source().id();
  EXPECT_TRUE(os::exists(volumePath));

  // Destroy the MOUNT disk with pipelined `CREATE`, `DESTROY` and `UNRESERVE`
  // operations. Note that `UNRESERVE` can come before `DESTROY_DISK`.
  Resource persistentVolume = created;
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_id(id::UUID::random().toString());
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_principal(framework.principal());
  persistentVolume.mutable_disk()->mutable_volume()
    ->set_container_path("volume");
  persistentVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

  Resource unreserved = *Resources(created)
    .popReservation()
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(raw)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume),
       DESTROY(persistentVolume),
       UNRESERVE(created),
       DESTROY_DISK(unreserved)});

  AWAIT_READY(offers);

  // Check if the volume is actually deleted by the test CSI plugin.
  EXPECT_FALSE(os::exists(volumePath));
}


// This test verifies that the storage local resource provider can destroy a
// MOUNT disk created from a storage pool with other pipelined operations after
// recovery.
TEST_P(StorageLocalResourceProviderTest, CreateDestroyDiskWithRecovery)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to exercise operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage/role");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  // Create a MOUNT disk with a pipelined `RESERVE` operation.
  Resource raw = *Resources(offer.resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .begin();

  Resource reserved = *Resources(raw)
    .pushReservation(createDynamicReservationInfo(
        framework.roles(0), framework.principal()))
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {RESERVE(reserved),
       CREATE_DISK(reserved, Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  Resource created = *Resources(offer.resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"))
    .begin();

  ASSERT_TRUE(created.disk().source().has_mount());
  ASSERT_TRUE(created.disk().source().mount().has_root());
  EXPECT_FALSE(path::is_absolute(created.disk().source().mount().root()));

  // Check if the volume is actually created by the test CSI plugin.
  const string& volumePath = created.disk().source().id();
  EXPECT_TRUE(os::exists(volumePath));

  // Restart the agent.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave.get()->terminate();

  // NOTE: We set up the resource provider with an extra storage pool, so that
  // when the storage pool shows up, we know that the resource provider has
  // finished reconciling storage pools and thus operations won't be dropped.
  //
  // To achieve this, we drop `SlaveRegisteredMessage`s other than the first one
  // to avoid unexpected `UpdateSlaveMessage`s. Then, we also drop the first two
  // `UpdateSlaveMessage`s (one sent after agent reregistration and one after
  // resource provider reregistration) and wait for the third one, which should
  // contain the extra storage pool. We let it fall through to trigger an offer
  // allocation for the persistent volume.
  //
  // Since the extra storage pool is never used, we reject the offers if only
  // the storage pool is presented.
  //
  // TODO(chhsiao): Remove this workaround once MESOS-9553 is done.
  setupResourceProviderConfig(Gigabytes(5));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // NOTE: The order of these expectations is reversed because Google Mock will
  // search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUFS(SlaveReregisteredMessage(), _, _);
  FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(created)))
    .WillOnce(FutureArg<1>(&offers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());
  ASSERT_FALSE(Resources(
      updateSlaveMessage->resource_providers().providers(0).total_resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .empty());

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Destroy the MOUNT disk with pipelined `CREATE`, `DESTROY` and `UNRESERVE`
  // operations. Note that `UNRESERVE` can come before `DESTROY_DISK`.
  Resource persistentVolume = created;
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_id(id::UUID::random().toString());
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_principal(framework.principal());
  persistentVolume.mutable_disk()->mutable_volume()
    ->set_container_path("volume");
  persistentVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

  Resource unreserved = *Resources(created)
    .popReservation()
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(raw)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume),
       DESTROY(persistentVolume),
       UNRESERVE(created),
       DESTROY_DISK(unreserved)});

  AWAIT_READY(offers);

  // Check if the volume is actually deleted by the test CSI plugin.
  EXPECT_FALSE(os::exists(volumePath));
}


// This test verifies that the storage local resource provider can properly
// handle changes in volume metadata. This is a regression test for MESOS-9395.
TEST_P(StorageLocalResourceProviderTest, RecoverDiskWithChangedMetadata)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  // Add metadata "label=foo" to each created volume.
  setupResourceProviderConfig(
      Gigabytes(4), None(), None(), None(), "label=foo");

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Register a framework to exercise operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage/role");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      &Resources::hasResourceProvider)))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  // Create a MOUNT disk.
  RepeatedPtrField<Resource> raw =
    Resources(offer.resources()).filter(&Resources::hasResourceProvider);

  ASSERT_EQ(1, raw.size());
  ASSERT_TRUE(isStoragePool(raw[0], "test"));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      &Resources::hasResourceProvider)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {CREATE_DISK(raw[0], Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  RepeatedPtrField<Resource> created =
    Resources(offer.resources()).filter(&Resources::hasResourceProvider);

  ASSERT_EQ(1, created.size());
  ASSERT_TRUE(isMountDisk(created[0], "test"));
  EXPECT_TRUE(created[0].disk().source().has_metadata());

  // Restart the agent.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave.get()->terminate();

  // Add metadata "label=bar" to each created volume. This dose not conform to
  // the CSI specification in terms of backward compatibility, but Mesos should
  // be robust to handle changes in the metadata.
  setupResourceProviderConfig(
      Gigabytes(4), None(), None(), None(), "label=bar");

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      Resources::hasResourceProvider)))
    .WillOnce(FutureArg<1>(&offers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  RepeatedPtrField<Resource> recovered =
    Resources(offer.resources()).filter(&Resources::hasResourceProvider);

  ASSERT_EQ(1, recovered.size()) << "To many resources: " << recovered;
  ASSERT_TRUE(isMountDisk(recovered[0], "test"));
  ASSERT_TRUE(recovered[0].disk().source().has_metadata());
  EXPECT_NE(created[0].disk().source().metadata(),
            recovered[0].disk().source().metadata())
    << "'" << JSON::protobuf(created[0].disk().source().metadata()) << "' vs "
       "'" << JSON::protobuf(recovered[0].disk().source().metadata()) << "'";
}


// This test verifies that the storage local resource provider can properly
// handle duplicated volumes. This is a regression test for MESOS-9965.
TEST_P(StorageLocalResourceProviderTest, RecoverDuplicatedVolumes)
{
  const string mockCsiEndpoint =
    "unix://" + path::join(sandbox.get(), "mock_csi.sock");

  MockCSIPlugin plugin;
  ASSERT_SOME(plugin.startup(mockCsiEndpoint));

  setupResourceProviderConfig(Bytes(0), None(), mockCsiEndpoint);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  if (GetParam() == csi::v0::API_VERSION) {
    EXPECT_CALL(plugin, ListVolumes(_, _, A<csi::v0::ListVolumesResponse*>()))
      .WillRepeatedly(Invoke([&](
          grpc::ServerContext* context,
          const csi::v0::ListVolumesRequest* request,
          csi::v0::ListVolumesResponse* response) {
        csi::v0::Volume volume;
        volume.set_capacity_bytes(Gigabytes(2).bytes());
        volume.set_id("volume1");

        // Report duplicated volumes.
        *response->add_entries()->mutable_volume() = volume;
        *response->add_entries()->mutable_volume() = volume;

        return grpc::Status::OK;
      }));
  } else {
    EXPECT_CALL(plugin, ListVolumes(_, _, A<csi::v1::ListVolumesResponse*>()))
      .WillRepeatedly(Invoke([&](
          grpc::ServerContext* context,
          const csi::v1::ListVolumesRequest* request,
          csi::v1::ListVolumesResponse* response) {
        csi::v1::Volume volume;
        volume.set_capacity_bytes(Gigabytes(2).bytes());
        volume.set_volume_id("volume1");

        // Report duplicated volumes.
        *response->add_entries()->mutable_volume() = volume;
        *response->add_entries()->mutable_volume() = volume;

        return grpc::Status::OK;
      }));
  }

  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Register a framework to exercise operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage/role");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      &Resources::hasResourceProvider)))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  // Restart the agent.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave.get()->terminate();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      &Resources::hasResourceProvider)))
    .WillOnce(FutureArg<1>(&offers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Wait for an offer to verify that the resource provider comes back.
  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  Resources resourceProviderResources =
    Resources(offer.resources()).filter(&Resources::hasResourceProvider);

  EXPECT_SOME_EQ(Gigabytes(2), resourceProviderResources.disk());
}


// This test verifies that a framework cannot create a volume during and after
// the profile disappears, and destroying a volume with a stale profile will
// recover the freed disk with another appeared profile.
//
// To accomplish this:
//   1. Create a 2GB MOUNT disk from a 4GB RAW disk of profile 'test1'.
//   2. Create another MOUNT disk from the rest RAW disk of profile 'test1'.
//   3. Remove profile 'test1' and adds profile 'test2' before the second
//      operation is applied. The operation would then be dropped, and the rest
//      RAW disk would show up as of profile 'test2'.
//   4. Destroy the MOUNT disk of profile 'test1'. All 4GB RAW disk would show
//      up as of profile 'test2'.
TEST_P(StorageLocalResourceProviderTest, ProfileDisappeared)
{
  Clock::pause();

  Future<shared_ptr<TestDiskProfileServer>> server =
    TestDiskProfileServer::create();

  AWAIT_READY(server);

  Promise<http::Response> updatedProfileMapping;
  EXPECT_CALL(*server.get()->process, profiles(_))
    .WillOnce(Return(http::OK(createDiskProfileMapping({{"test1", None()}}))))
    .WillOnce(Return(updatedProfileMapping.future()))
    .WillRepeatedly(Return(Future<http::Response>())); // Stop subsequent polls.

  const Duration pollInterval = Seconds(10);
  loadUriDiskProfileAdaptorModule(
      stringify(server.get()->process->url()),
      pollInterval);

  setupResourceProviderConfig(Gigabytes(4));

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider gets subscribed after the agent
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

  // Register a v1 framework to exercise operations with feedback.
  v1::FrameworkInfo framework = v1::DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(framework));

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(v1::scheduler::DeclineOffers());

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(
      *scheduler,
      offers(_, v1::scheduler::OffersHaveAnyResource(
          std::bind(isStoragePool<v1::Resource>, lambda::_1, "test1"))))
    .WillOnce(FutureArg<1>(&offers));

  v1::scheduler::TestMesos mesos(
      master.get()->pid, ContentType::PROTOBUF, scheduler);

  AWAIT_READY(subscribed);

  const v1::FrameworkID& frameworkId = subscribed->framework_id();

  // NOTE: If the framework has not declined an unwanted offer yet when the
  // resource provider reports its RAW resources, the new allocation triggered
  // by this update won't generate an allocatable offer due to no CPU and memory
  // resources. So we first settle the clock to ensure that the unwanted offer
  // has been declined, then advance the clock to trigger another allocation.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_EQ(1, offers->offers_size());

  v1::Offer offer = offers->offers(0);

  EXPECT_CALL(
      *scheduler,
      offers(_, v1::scheduler::OffersHaveAnyResource(
          std::bind(isStoragePool<v1::Resource>, lambda::_1, "test1"))))
    .WillOnce(FutureArg<1>(&offers));

  // Create a 2GB MOUNT disk of profile 'test1'.
  {
    v1::Resources raw = v1::Resources(offer.resources())
      .filter(std::bind(isStoragePool<v1::Resource>, lambda::_1, "test1"));

    ASSERT_SOME_EQ(Gigabytes(4), raw.disk());

    // Just use 2GB of the storage pool.
    v1::Resource source = *raw.begin();
    source.mutable_scalar()->set_value(
        static_cast<double>(Gigabytes(2).bytes()) / Bytes::MEGABYTES);

    v1::OperationID operationId;
    operationId.set_value(id::UUID::random().toString());

    Future<v1::scheduler::Event::UpdateOperationStatus> update;
    EXPECT_CALL(
        *scheduler,
        updateOperationStatus(
            _, v1::scheduler::OperationStatusUpdateOperationIdEq(operationId)))
      .WillOnce(FutureArg<1>(&update))
      .WillRepeatedly(Return()); // Ignore subsequent updates.

    mesos.send(v1::createCallAccept(
        frameworkId,
        offer,
        {v1::CREATE_DISK(
             source,
             v1::Resource::DiskInfo::Source::MOUNT,
             None(),
             operationId)}));

    AWAIT_READY(update);
    EXPECT_EQ(v1::OPERATION_FINISHED, update->status().state());
  }

  // Advance the clock to trigger another allocation.
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_EQ(1, offers->offers_size());

  offer = offers->offers(0);

  EXPECT_CALL(
      *scheduler,
      offers(_, v1::scheduler::OffersHaveAnyResource(
          std::bind(isMountDisk<v1::Resource>, lambda::_1, "test1"))))
    .WillOnce(FutureArg<1>(&offers));

  // We drop the agent update (which is triggered by the changes in the known
  // set of profiles) to simulate the situation where the update races with
  // an offer operation.
  Future<UpdateSlaveMessage> updateSlave3 =
    DROP_PROTOBUF(UpdateSlaveMessage(), _, _);

  // Remove profile 'test1' and add profile 'test2'. No allocation will be
  // triggered since the framework is still holding the current offer.
  Clock::advance(pollInterval);
  updatedProfileMapping.set(
      http::OK(createDiskProfileMapping({{"test2", None()}})));

  AWAIT_READY(updateSlave3);

  // Create another MOUNT disk from the rest RAW disk of profile 'test1'. This
  // operation would be dropped due to a mismatched resource version.
  {
    v1::Resources raw = v1::Resources(offer.resources())
      .filter(std::bind(isStoragePool<v1::Resource>, lambda::_1, "test1"));

    ASSERT_SOME_EQ(Gigabytes(2), raw.disk());

    v1::OperationID operationId;
    operationId.set_value(id::UUID::random().toString());

    Future<v1::scheduler::Event::UpdateOperationStatus> update;
    EXPECT_CALL(
        *scheduler,
        updateOperationStatus(
            _, v1::scheduler::OperationStatusUpdateOperationIdEq(operationId)))
      .WillOnce(FutureArg<1>(&update))
      .WillRepeatedly(Return()); // Ignore subsequent updates;

    mesos.send(v1::createCallAccept(
        frameworkId,
        offer,
        {v1::CREATE_DISK(
             *raw.begin(),
             v1::Resource::DiskInfo::Source::MOUNT,
             None(),
             operationId)}));

    AWAIT_READY(update);
    EXPECT_EQ(v1::OPERATION_DROPPED, update->status().state());
  }

  // Forward the dropped agent update to trigger another allocation.
  post(slave.get()->pid, master.get()->pid, updateSlave3.get());

  AWAIT_READY(offers);
  ASSERT_EQ(1, offers->offers_size());

  offer = offers->offers(0);

  EXPECT_CALL(
      *scheduler,
      offers(_, v1::scheduler::OffersHaveAnyResource(
          std::bind(isStoragePool<v1::Resource>, lambda::_1, "test2"))))
    .WillOnce(FutureArg<1>(&offers));

  // Destroy the MOUNT disk of profile 'test1'. The returned converted resources
  // should be empty.
  {
    v1::Resources created = v1::Resources(offer.resources())
      .filter(std::bind(isMountDisk<v1::Resource>, lambda::_1, "test1"));

    ASSERT_SOME_EQ(Gigabytes(2), created.disk());

    v1::OperationID operationId;
    operationId.set_value(id::UUID::random().toString());

    Future<v1::scheduler::Event::UpdateOperationStatus> update;
    EXPECT_CALL(
        *scheduler,
        updateOperationStatus(
            _, v1::scheduler::OperationStatusUpdateOperationIdEq(operationId)))
      .WillOnce(FutureArg<1>(&update))
      .WillRepeatedly(Return()); // Ignore subsequent updates;

    mesos.send(v1::createCallAccept(
        frameworkId, offer, {v1::DESTROY_DISK(*created.begin(), operationId)}));

    AWAIT_READY(update);
    EXPECT_EQ(v1::OPERATION_FINISHED, update->status().state());
    EXPECT_TRUE(update->status().converted_resources().empty());
  }

  // The resource provider will reconcile the storage pools to reclaim the
  // space freed by destroying a MOUNT disk of a disappeared profile, which
  // would in turn trigger another agent update and thus another allocation.
  AWAIT_READY(offers);
  ASSERT_EQ(1, offers->offers_size());

  offer = offers->offers(0);

  // Check that the freed disk shows up as of profile 'test2'.
  {
    v1::Resources raw = v1::Resources(offer.resources())
      .filter(std::bind(isStoragePool<v1::Resource>, lambda::_1, "test2"));

    EXPECT_SOME_EQ(Gigabytes(4), raw.disk());
  }
}


// This test verifies that the storage local resource provider can
// recover if the plugin is killed during an agent failover..
TEST_P(StorageLocalResourceProviderTest, AgentFailoverPluginKilled)
{
  setupResourceProviderConfig(Bytes(0), "volume0:4GB");

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

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

  // Register a framework to receive offers.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The framework is expected to see the following offers in sequence:
  //   1. One containing any resource from the resource provider before the
  //      agent fails over.
  //   2. One containing any resource from the resource provider after the agent
  //      recovers from the failover.
  //
  // We set up the expectations for these offers as the test progresses.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> slaveRecoveredOffers;

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      &Resources::hasResourceProvider)))
    .WillOnce(FutureArg<1>(&rawDiskOffers));

  driver.start();

  AWAIT_READY(rawDiskOffers);

  // Get the PID of the plugin container so we can kill it later.
  Future<hashset<ContainerID>> pluginContainers = containerizer->containers();

  AWAIT_READY(pluginContainers);
  ASSERT_EQ(1u, pluginContainers->size());

  Future<ContainerStatus> pluginStatus =
    containerizer->status(*pluginContainers->begin());

  AWAIT_READY(pluginStatus);

  // Terminate the agent to simulate a failover.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave.get()->terminate();
  slave->reset();
  containerizer.reset();

  // Kill the plugin container.
  Future<Option<int>> pluginReaped = reap(pluginStatus->executor_pid());
  ASSERT_EQ(0, os::kill(pluginStatus->executor_pid(), SIGKILL));

  AWAIT_READY(pluginReaped);
  ASSERT_SOME(pluginReaped.get());
  ASSERT_TRUE(WIFSIGNALED(pluginReaped->get()));
  EXPECT_EQ(SIGKILL, WTERMSIG(pluginReaped->get()));

  // Restart the agent.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      &Resources::hasResourceProvider)))
    .WillOnce(FutureArg<1>(&slaveRecoveredOffers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRecoveredOffers);
}


// This test verifies that if an agent is registered with a new ID, the resource
// provider ID would change as well, and any created volume becomes a
// preprovisioned volume. A framework should be able to either create a MOUNT
// disk from a preprovisioned volume and use it, or destroy it directly.
TEST_P(StorageLocalResourceProviderTest, ROOT_AgentRegisteredWithNewId)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  // We set up a 'good' and a 'bad' profile to test that `CREATE_DISK` only
  // succeeds with the right profile. Ideally this should be tested in
  // `CreateDestroyPreprovisionedVolume`, but since CSI v0 doesn't support
  // validating a volume against parameters, we have to test it here.
  ASSERT_SOME(os::write(profilesPath, createDiskProfileMapping(
      {{"good", JSON::Object{{"label", "foo"}}},
       {"bad", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(5), None(), None(), "label=foo");

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
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

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers, e.g., offers containing only agent-default
  // resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // We first wait for an offer containing a storage pool to exercise two
  // `CREATE_DISK` operations. Since they are not done atomically, we might get
  // subsequent offers containing a smaller storage pool after one is exercised,
  // so we decline them.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "good"))))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers(declineFilters));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  Resources raw = Resources(offer.resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "good"));

  // Create a 2GB and a 3GB MOUNT disks.
  ASSERT_SOME_EQ(Gigabytes(5), raw.disk());
  Resource source1 = *raw.begin();
  source1.mutable_scalar()->set_value(
      static_cast<double>(Gigabytes(2).bytes()) / Bytes::MEGABYTES);

  raw -= source1;
  ASSERT_SOME_EQ(Gigabytes(3), raw.disk());
  Resource source2 = *raw.begin();

  // After the following operations are completed, we would get an offer
  // containing __two__ MOUNT disks: one 2GB and one 3GB.
  EXPECT_CALL(sched, resourceOffers(&driver, AllOf(
      OffersHaveAnyResource([](const Resource& r) {
        return isMountDisk(r, "good") &&
          Megabytes(r.scalar().value()) == Gigabytes(2);
      }),
      OffersHaveAnyResource([](const Resource& r) {
        return isMountDisk(r, "good") &&
          Megabytes(r.scalar().value()) == Gigabytes(3);
      }))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {CREATE_DISK(source1, Resource::DiskInfo::Source::MOUNT),
       CREATE_DISK(source2, Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  std::vector<Resource> created = google::protobuf::convert<Resource>(
      Resources(offer.resources())
        .filter(std::bind(isMountDisk<Resource>, lambda::_1, "good")));

  ASSERT_EQ(2u, created.size());

  // Create persistent MOUNT volumes then launch a tasks to write files.
  Resources persistentVolumes;
  hashmap<string, ResourceProviderID> sourceIdToProviderId;
  const vector<string> containerPaths = {"volume1", "volume2"};
  for (size_t i = 0; i < created.size(); i++) {
    const Resource& volume = created[i];
    ASSERT_TRUE(volume.has_provider_id());
    sourceIdToProviderId.put(volume.disk().source().id(), volume.provider_id());

    Resource persistentVolume = volume;
    persistentVolume.mutable_disk()->mutable_persistence()
      ->set_id(id::UUID::random().toString());
    persistentVolume.mutable_disk()->mutable_persistence()
      ->set_principal(framework.principal());
    persistentVolume.mutable_disk()->mutable_volume()
      ->set_container_path(containerPaths[i]);
    persistentVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

    persistentVolumes += persistentVolume;
  }

  Future<Nothing> taskFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_STARTING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_RUNNING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_FINISHED)))
    .WillOnce(FutureSatisfy(&taskFinished));

  // After the task is finished, we would get an offer containing the two
  // persistent MOUNT volumes. We just match any one here for simplicity.
  EXPECT_CALL( sched, resourceOffers(&driver, OffersHaveAnyResource(
      Resources::isPersistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolumes),
       LAUNCH({createTask(
           offer.slave_id(),
           createTaskResources(persistentVolumes),
           createCommandInfo(
               "touch " + path::join(containerPaths[0], "file") + " " +
               path::join(containerPaths[1], "file")))})});

  AWAIT_READY(taskFinished);

  AWAIT_READY(offers);

  // Shut down the agent.
  EXPECT_CALL(sched, offerRescinded(&driver, _));

  slave.get()->terminate();

  // Remove the `latest` symlink to register the agent with a new ID.
  const string metaDir = slave::paths::getMetaRootDir(slaveFlags.work_dir);
  ASSERT_SOME(os::rm(slave::paths::getLatestSlavePath(metaDir)));

  // NOTE: We set up the resource provider with an extra storage pool, so that
  // when the storage pool shows up, we know that the resource provider has
  // finished reconciling storage pools and thus operations won't be dropped.
  //
  // To achieve this, we drop `SlaveRegisteredMessage`s other than the first one
  // to avoid unexpected `UpdateSlaveMessage`s. Then, we also drop the first two
  // `UpdateSlaveMessage`s (one sent after agent registration and one after
  // resource provider registration) and wait for the third one, which should
  // contain the extra storage pool. We let it fall through to trigger an offer
  // allocation for the persistent volume.
  //
  // TODO(chhsiao): Remove this workaround once MESOS-9553 is done.
  setupResourceProviderConfig(Gigabytes(6), None(), None(), "label=foo");

  // NOTE: The order of these expectations is reversed because Google Mock will
  // search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUFS(SlaveRegisteredMessage(), _, _);

  Future<SlaveRegisteredMessage> newSlaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, Not(slave.get()->pid));

  // After the new agent starts, we wait for an offer containing the two
  // preprovisioned volumes to exercise two `CREATE_DISK` operations. Since one
  // of them would fail, we might get subsequent offers containing a
  // preprovisioned volumes afterwards, so we decline them.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      isPreprovisionedVolume<Resource>)))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers(declineFilters));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(newSlaveRegisteredMessage);
  ASSERT_NE(slaveRegisteredMessage->slave_id(),
            newSlaveRegisteredMessage->slave_id());

  AWAIT_READY(updateSlaveMessage);
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());
  ASSERT_FALSE(Resources(
      updateSlaveMessage->resource_providers().providers(0).total_resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "good"))
    .empty());

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  vector<Resource> preprovisioned = google::protobuf::convert<Resource>(
      Resources(offer.resources()).filter(isPreprovisionedVolume<Resource>));

  ASSERT_EQ(2u, preprovisioned.size());

  // Check that the resource provider IDs of the volumes have changed.
  vector<string> volumePaths;
  foreach (const Resource& volume, preprovisioned) {
    ASSERT_TRUE(volume.has_provider_id());
    ASSERT_TRUE(sourceIdToProviderId.contains(volume.disk().source().id()));
    ASSERT_NE(sourceIdToProviderId.at(volume.disk().source().id()),
              volume.provider_id());

    const string& volumePath = volume.disk().source().id();
    ASSERT_TRUE(os::exists(volumePath));
    volumePaths.push_back(volumePath);
  }

  // Apply profiles 'good' and 'bad' to the preprovisioned volumes. The first
  // operation would succeed but the second would fail.
  Future<multiset<OperationState>> createDiskOperationStates =
    process::collect(
        FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _),
        FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _))
      .then([](const std::tuple<
                UpdateOperationStatusMessage,
                UpdateOperationStatusMessage>& operationStatuses) {
        return multiset<OperationState>{
          std::get<0>(operationStatuses).status().state(),
          std::get<1>(operationStatuses).status().state()};
      });

  // After both operations are completed, we would get an offer containing both
  // a MOUNT disk and a preprovisioned volume.
  EXPECT_CALL(sched, resourceOffers(&driver, AllOf(
      OffersHaveAnyResource(
          std::bind(isMountDisk<Resource>, lambda::_1, "good")),
      OffersHaveResource(preprovisioned[1]))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {CREATE_DISK(
           preprovisioned[0], Resource::DiskInfo::Source::MOUNT, "good"),
       CREATE_DISK(
           preprovisioned[1], Resource::DiskInfo::Source::MOUNT, "bad")});

  multiset<OperationState> expectedOperationStates = {
    OperationState::OPERATION_FINISHED,
    OperationState::OPERATION_FAILED};

  AWAIT_EXPECT_EQ(expectedOperationStates, createDiskOperationStates);

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  Resource imported = *Resources(offer.resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "good"))
    .begin();

  // Create persistent MOUNT volumes from the imported volume then launch a
  // tasks to write files, and destroy the other unimported volume.
  imported.mutable_disk()->mutable_persistence()
    ->set_id(id::UUID::random().toString());
  imported.mutable_disk()->mutable_persistence()
    ->set_principal(framework.principal());
  imported.mutable_disk()->mutable_volume()->set_container_path("volume");
  imported.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_STARTING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_RUNNING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_FINISHED)))
    .WillOnce(FutureSatisfy(&taskFinished));

  Future<UpdateOperationStatusMessage> destroyDiskOperationStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  // After the task is finished and the operation is completed, we would get an
  // offer the persistent volume and a storage pool that has the freed space.
  EXPECT_CALL(sched, resourceOffers(&driver, AllOf(
      OffersHaveResource(imported),
      OffersHaveAnyResource([](const Resource& r) {
        return isStoragePool(r, "good") &&
          Megabytes(r.scalar().value()) >= Gigabytes(2);
      }))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(imported),
       LAUNCH({createTask(
           offer.slave_id(),
           createTaskResources(imported),
           createCommandInfo("test -f " + path::join("volume", "file")))}),
       DESTROY_DISK(preprovisioned[1])});

  AWAIT_READY(taskFinished);

  AWAIT_READY(destroyDiskOperationStatus);
  EXPECT_EQ(OperationState::OPERATION_FINISHED,
            destroyDiskOperationStatus->status().state());

  AWAIT_READY(offers);

  // Check if the unimported volume is deleted by the test CSI plugin.
  EXPECT_FALSE(os::exists(volumePaths[1]));
}


// This test verifies that the storage local resource provider can create and
// publish a persistent volume used by a task, and the persistent volume will be
// cleaned up when being destroyed.
//
// To accomplish this:
//   1. Create a MOUNT disk from a RAW disk resource.
//   2. Create a persistent volume on the MOUNT disk then launches a task to
//      write a file into it.
//   3. Destroy the persistent volume but keep the MOUNT disk. The file should
//      be deleted.
//   4. Destroy the MOUNT disk.
TEST_P(
    StorageLocalResourceProviderTest, ROOT_CreateDestroyPersistentMountVolume)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
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

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  // Create a MOUNT disk.
  Resource raw = *Resources(offer.resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()}, {CREATE_DISK(raw, Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  Resource created = *Resources(offer.resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"))
    .begin();

  ASSERT_TRUE(created.disk().source().has_mount());
  ASSERT_TRUE(created.disk().source().mount().has_root());
  EXPECT_FALSE(path::is_absolute(created.disk().source().mount().root()));

  // Check if the CSI volume is actually created.
  const string& volumePath = created.disk().source().id();
  EXPECT_TRUE(os::exists(volumePath));

  // Create a persistent MOUNT volume then launch a task to write a file.
  Resource persistentVolume = created;
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_id(id::UUID::random().toString());
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_principal(framework.principal());
  persistentVolume.mutable_disk()->mutable_volume()
    ->set_container_path("volume");
  persistentVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

  Future<Nothing> taskFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_STARTING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_RUNNING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_FINISHED)))
    .WillOnce(FutureSatisfy(&taskFinished));

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume),
       LAUNCH({createTask(
           offer.slave_id(),
           createTaskResources(persistentVolume),
           createCommandInfo("touch " + path::join("volume", "file")))})});

  AWAIT_READY(taskFinished);

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Destroy the persistent volume.
  Future<UpdateOperationStatusMessage> updateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(created)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers({offer.id()}, {DESTROY(persistentVolume)});

  // NOTE: Since `DESTROY` would be applied by the master synchronously, we
  // might get an offer before the persistent volume is cleaned up on the agent,
  // so we wait for an `UpdateOperationStatusMessage` before the check below.
  AWAIT_READY(updateOperationStatusMessage);
  EXPECT_EQ(OPERATION_FINISHED, updateOperationStatusMessage->status().state());

  // Check if the CSI volume still exists but has being cleaned up.
  EXPECT_TRUE(os::exists(volumePath));
  EXPECT_FALSE(os::exists(path::join(volumePath, "file")));

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Destroy the MOUNT disk.
  updateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(raw)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers({offer.id()}, {DESTROY_DISK(created)});

  AWAIT_READY(updateOperationStatusMessage);
  EXPECT_EQ(OPERATION_FINISHED, updateOperationStatusMessage->status().state());

  // Check if the CSI volume is actually deleted.
  EXPECT_FALSE(os::exists(volumePath));

  AWAIT_READY(offers);
}


// This test verifies that the storage local resource provider can republish and
// clean up persistent volumes after recovery.
//
// To accomplish this:
//   1. Create a MOUNT disk from a RAW disk resources.
//   2. Create a persistent volume on the MOUNT disk then launch a task to
//      write a file into it.
//   3. Restart the agent.
//   4. Launch another task to read the file.
//   5. Restart the agent again.
//   6. Destroy the persistent volume but keep the MOUNT disk. The file should
//      be deleted.
//   7. Destroy the MOUNT disk.
TEST_P(
    StorageLocalResourceProviderTest,
    ROOT_CreateDestroyPersistentMountVolumeWithRecovery)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
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

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  // Create a MOUNT disk.
  Resource raw = *Resources(offer.resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()}, {CREATE_DISK(raw, Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  Resource created = *Resources(offer.resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"))
    .begin();

  // Create a persistent MOUNT volume then launch a task to write a file.
  Resource persistentVolume = created;
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_id(id::UUID::random().toString());
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_principal(framework.principal());
  persistentVolume.mutable_disk()->mutable_volume()
    ->set_container_path("volume");
  persistentVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

  Future<Nothing> taskFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_STARTING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_RUNNING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_FINISHED)))
    .WillOnce(FutureSatisfy(&taskFinished));

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume),
       LAUNCH({createTask(
           offer.slave_id(),
           createTaskResources(persistentVolume),
           createCommandInfo("touch " + path::join("volume", "file")))})});

  AWAIT_READY(taskFinished);

  AWAIT_READY(offers);

  // Restart the agent.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave.get()->terminate();

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Launch another task to read the file created by the previous task.
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_STARTING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_RUNNING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_FINISHED)))
    .WillOnce(FutureSatisfy(&taskFinished));

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {LAUNCH({createTask(
           offer.slave_id(),
           createTaskResources(persistentVolume),
           createCommandInfo("test -f " + path::join("volume", "file")))})});

  AWAIT_READY(taskFinished);

  AWAIT_READY(offers);

  // Restart the agent.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave.get()->terminate();

  // NOTE: We set up the resource provider with an extra storage pool, so that
  // when the storage pool shows up, we know that the resource provider has
  // finished reconciling storage pools and thus operations won't be dropped.
  //
  // To achieve this, we drop `SlaveRegisteredMessage`s other than the first one
  // to avoid unexpected `UpdateSlaveMessage`s. Then, we also drop the first two
  // `UpdateSlaveMessage`s (one sent after agent reregistration and one after
  // resource provider reregistration) and wait for the third one, which should
  // contain the extra storage pool. We let it fall through to trigger an offer
  // allocation for the persistent volume.
  //
  // Since the extra storage pool is never used, we reject the offers if only
  // the storage pool is presented.
  //
  // TODO(chhsiao): Remove this workaround once MESOS-9553 is done.
  setupResourceProviderConfig(Gigabytes(5));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // NOTE: The order of these expectations is reversed because Google Mock will
  // search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUFS(SlaveReregisteredMessage(), _, _);
  FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());
  ASSERT_FALSE(Resources(
      updateSlaveMessage->resource_providers().providers(0).total_resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .empty());

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Destroy the persistent volume.
  Future<UpdateOperationStatusMessage> updateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(created)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers({offer.id()}, {DESTROY(persistentVolume)});

  // NOTE: Since `DESTROY` would be applied by the master synchronously, we
  // might get an offer before the persistent volume is cleaned up on the agent,
  // so we wait for an `UpdateOperationStatusMessage` before the check below.
  AWAIT_READY(updateOperationStatusMessage);
  EXPECT_EQ(OPERATION_FINISHED, updateOperationStatusMessage->status().state());

  // Check if the CSI volume still exists but has being cleaned up.
  const string& volumePath = created.disk().source().id();
  EXPECT_TRUE(os::exists(volumePath));
  EXPECT_FALSE(os::exists(path::join(volumePath, "file")));

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Destroy the MOUNT disk.
  updateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(raw)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers({offer.id()}, {DESTROY_DISK(created)});

  AWAIT_READY(updateOperationStatusMessage);
  EXPECT_EQ(OPERATION_FINISHED, updateOperationStatusMessage->status().state());

  // Check if the CSI volume is actually deleted.
  EXPECT_FALSE(os::exists(volumePath));

  AWAIT_READY(offers);
}


// This test verifies that the storage local resource provider can republish and
// clean up persistent volumes after agent reboot.
//
// To accomplish this:
//   1. Create a MOUNT disk from a RAW disk resource.
//   2. Create a persistent volume on the MOUNT disk then launches a task to
//      write a file into it.
//   3. Simulate an agent reboot.
//   4. Launch another task to read the file.
//   5. Simulate another agent reboot.
//   6. Destroy the persistent volume but keep the MOUNT disk. The file should
//      be deleted.
//   7. Destroy the MOUNT disk.
TEST_P(
    StorageLocalResourceProviderTest,
    ROOT_CreateDestroyPersistentMountVolumeWithReboot)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
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

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  // Create a MOUNT disk.
  Resource raw = *Resources(offer.resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()}, {CREATE_DISK(raw, Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  Resource created = *Resources(offer.resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"))
    .begin();

  // Create a persistent MOUNT volume then launch a task to write a file.
  Resource persistentVolume = created;
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_id(id::UUID::random().toString());
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_principal(framework.principal());
  persistentVolume.mutable_disk()->mutable_volume()
    ->set_container_path("volume");
  persistentVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

  Future<Nothing> taskFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_STARTING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_RUNNING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_FINISHED)))
    .WillOnce(FutureSatisfy(&taskFinished));

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume),
       LAUNCH({createTask(
           offer.slave_id(),
           createTaskResources(persistentVolume),
           createCommandInfo("touch " + path::join("volume", "file")))})});

  AWAIT_READY(taskFinished);

  AWAIT_READY(offers);

  // Shutdown the agent and unmount all CSI volumes to simulate the 1st reboot.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave->reset();

  const string csiRootDir = slave::paths::getCsiRootDir(slaveFlags.work_dir);
  ASSERT_SOME(fs::unmountAll(csiRootDir));

  // Inject the boot IDs to simulate the 1st reboot.
  auto injectBootIds = [&](const string& bootId) {
    ASSERT_SOME(os::write(
        slave::paths::getBootIdPath(
            slave::paths::getMetaRootDir(slaveFlags.work_dir)),
        bootId));

    Try<list<string>> volumePaths =
      csi::paths::getVolumePaths(csiRootDir, "*", "*");
    ASSERT_SOME(volumePaths);
    ASSERT_FALSE(volumePaths->empty());

    foreach (const string& path, volumePaths.get()) {
      Try<csi::paths::VolumePath> volumePath =
        csi::paths::parseVolumePath(csiRootDir, path);
      ASSERT_SOME(volumePath);

      const string volumeStatePath = csi::paths::getVolumeStatePath(
          csiRootDir, volumePath->type, volumePath->name, volumePath->volumeId);

      Result<csi::state::VolumeState> volumeState =
        slave::state::read<csi::state::VolumeState>(volumeStatePath);

      ASSERT_SOME(volumeState);

      if (volumeState->state() == csi::state::VolumeState::PUBLISHED) {
        volumeState->set_boot_id(bootId);
        ASSERT_SOME(
            slave::state::checkpoint(volumeStatePath, volumeState.get()));
      }
    }
  };

  injectBootIds("1st reboot! ;)");

  // Restart the agent.
  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Launch another task to read the file created by the previous task.
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_STARTING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_RUNNING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_FINISHED)))
    .WillOnce(FutureSatisfy(&taskFinished));

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {LAUNCH({createTask(
           offer.slave_id(),
           createTaskResources(persistentVolume),
           createCommandInfo("test -f " + path::join("volume", "file")))})});

  AWAIT_READY(taskFinished);

  AWAIT_READY(offers);

  // Shutdown the agent and unmount all CSI volumes to simulate the 2nd reboot.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave->reset();
  ASSERT_SOME(fs::unmountAll(csiRootDir));

  // Inject the boot IDs to simulate the 2nd reboot.
  injectBootIds("2nd reboot! ;)");

  // NOTE: We set up the resource provider with an extra storage pool, so that
  // when the storage pool shows up, we know that the resource provider has
  // finished reconciling storage pools and thus operations won't be dropped.
  //
  // To achieve this, we drop `SlaveRegisteredMessage`s other than the first one
  // to avoid unexpected `UpdateSlaveMessage`s. Then, we also drop the first two
  // `UpdateSlaveMessage`s (one sent after agent reregistration and one after
  // resource provider reregistration) and wait for the third one, which should
  // contain the extra storage pool. We let it fall through to trigger an offer
  // allocation for the persistent volume.
  //
  // Since the extra storage pool is never used, we reject the offers if only
  // the storage pool is presented.
  //
  // TODO(chhsiao): Remove this workaround once MESOS-9553 is done.
  setupResourceProviderConfig(Gigabytes(5));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // NOTE: The order of these expectations is reversed because Google Mock will
  // search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUFS(SlaveReregisteredMessage(), _, _);
  FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the agent.
  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());
  ASSERT_FALSE(Resources(
      updateSlaveMessage->resource_providers().providers(0).total_resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .empty());

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Destroy the persistent volume.
  Future<UpdateOperationStatusMessage> updateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(created)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers({offer.id()}, {DESTROY(persistentVolume)});

  // NOTE: Since `DESTROY` would be applied by the master synchronously, we
  // might get an offer before the persistent volume is cleaned up on the agent,
  // so we wait for an `UpdateOperationStatusMessage` before the check below.
  AWAIT_READY(updateOperationStatusMessage);
  EXPECT_EQ(OPERATION_FINISHED, updateOperationStatusMessage->status().state());

  // Check if the CSI volume still exists but has being cleaned up.
  const string& volumePath = created.disk().source().id();
  EXPECT_TRUE(os::exists(volumePath));
  EXPECT_FALSE(os::exists(path::join(volumePath, "file")));

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Destroy the MOUNT disk.
  updateOperationStatusMessage =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(raw)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers({offer.id()}, {DESTROY_DISK(created)});

  AWAIT_READY(updateOperationStatusMessage);
  EXPECT_EQ(OPERATION_FINISHED, updateOperationStatusMessage->status().state());

  // Check if the CSI volume is actually deleted.
  EXPECT_FALSE(os::exists(volumePath));

  AWAIT_READY(offers);
}


// This test verifies that the storage local resource provider can
// restart its CSI plugin after it is killed and continue to work
// properly.
TEST_P(
    StorageLocalResourceProviderTest,
    ROOT_PublishUnpublishResourcesPluginKilled)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.isolation = "filesystem/linux";

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
  //   1. One containing a RAW disk resource before `CREATE_DISK`.
  //   2. One containing a MOUNT disk resource after `CREATE_DISK`.
  //   3. One containing the same MOUNT disk resource after `CREATE`,
  //      `LAUNCH` and `DESTROY`.
  //   4. One containing the same RAW disk resource after `DESTROY_DISK`.
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

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&rawDiskOffers));

  driver.start();

  AWAIT_READY(rawDiskOffers);
  ASSERT_FALSE(rawDiskOffers->empty());

  Option<Resource> source;

  foreach (const Resource& resource, rawDiskOffers->at(0).resources()) {
    if (isStoragePool(resource, "test")) {
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
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeCreatedOffers));

  driver.acceptOffers(
      {rawDiskOffers->at(0).id()},
      {CREATE_DISK(source.get(), Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(volumeCreatedOffers);
  ASSERT_FALSE(volumeCreatedOffers->empty());

  Option<Resource> volume;

  foreach (const Resource& resource, volumeCreatedOffers->at(0).resources()) {
    if (isMountDisk(resource, "test")) {
      volume = resource;
      break;
    }
  }

  ASSERT_SOME(volume);
  ASSERT_TRUE(volume->disk().source().has_vendor());
  EXPECT_EQ(TEST_CSI_VENDOR, volume->disk().source().vendor());
  ASSERT_TRUE(volume->disk().source().has_id());
  ASSERT_TRUE(volume->disk().source().has_mount());
  ASSERT_TRUE(volume->disk().source().mount().has_root());
  EXPECT_FALSE(path::is_absolute(volume->disk().source().mount().root()));

  // Check if the volume is actually created by the test CSI plugin.
  const string& volumePath = volume->disk().source().id();
  EXPECT_TRUE(os::exists(volumePath));

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
  ASSERT_SOME(os::touch(path::join(volumePath, "file")));

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
           createTaskResources(persistentVolume),
           createCommandInfo("test -f " + path::join("volume", "file")))})});

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
      {DESTROY(persistentVolume), DESTROY_DISK(volume.get())});

  AWAIT_READY(volumeDestroyedOffers);
  ASSERT_FALSE(volumeDestroyedOffers->empty());

  // Check if the volume is actually deleted by the test CSI plugin.
  EXPECT_FALSE(os::exists(volumePath));
}


// This test verifies that the storage local resource provider would fail to
// create a persistent volume on a BLOCK disk resource.
//
// TODO(chhsiao): Update this test once persistent BLOCK volumes are supported.
TEST_P(StorageLocalResourceProviderTest, CreatePersistentBlockVolume)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");
  Try<string> blockDiskProfileMapping = strings::format(
      R"~(
      {
        "profile_matrix": {
          "test": {
            "csi_plugin_type_selector": {
              "plugin_type": "%s"
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
      )~",
      TEST_CSI_PLUGIN_TYPE);

  ASSERT_SOME(blockDiskProfileMapping);
  ASSERT_SOME(os::write(profilesPath, blockDiskProfileMapping.get()));

  loadUriDiskProfileAdaptorModule(profilesPath);

  const string mockCsiEndpoint =
    "unix://" + path::join(sandbox.get(), "mock_csi.sock");

  // We use a mock CSI plugin here because the test CSI plugin does not support
  // block volumes yet.
  MockCSIPlugin plugin;
  ASSERT_SOME(plugin.startup(mockCsiEndpoint));

  setupResourceProviderConfig(Bytes(0), None(), mockCsiEndpoint);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
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

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  // Create a BLOCK disk.
  Resource raw = *Resources(offer.resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isBlockDisk<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()}, {CREATE_DISK(raw, Resource::DiskInfo::Source::BLOCK)});

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  Resource created = *Resources(offer.resources())
    .filter(std::bind(isBlockDisk<Resource>, lambda::_1, "test"))
    .begin();

  // Create a persistent BLOCK volume, which would fail.
  Resource persistentVolume = created;
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_id(id::UUID::random().toString());
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_principal(framework.principal());
  persistentVolume.mutable_disk()->mutable_volume()
    ->set_container_path("volume");
  persistentVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

  Future<UpdateOperationStatusMessage> createOperationStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(created)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers({offer.id()}, {CREATE(persistentVolume)});

  AWAIT_READY(createOperationStatus);
  EXPECT_EQ(OPERATION_FAILED, createOperationStatus->status().state());

  AWAIT_EXPECT_READY(offers)
    << "Failed to wait for an offer containing resource '" << created << "'";
}


// This test verifies that if a persistent volumes is never published by the
// storage local resource provider, the volume can be destroyed.
//
// To accomplish this:
//   1. Create a MOUNT disk from a RAW disk resource.
//   2. Create a persistent volume on the MOUNT disk then launches a task to
//      write a file into it.
//   3. Return `UNIMPLEMENTED` for the `NodePublishVolume` call. The task will
//      fail to launch.
//   4. Destroy the persistent volume and the MOUNT disk.
TEST_P(StorageLocalResourceProviderTest, DestroyUnpublishedPersistentVolume)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  const string mockCsiEndpoint =
    "unix://" + path::join(sandbox.get(), "mock_csi.sock");

  MockCSIPlugin plugin;
  ASSERT_SOME(plugin.startup(mockCsiEndpoint));

  setupResourceProviderConfig(Bytes(0), None(), mockCsiEndpoint);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
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

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  // Create a MOUNT disk.
  Resource raw = *Resources(offer.resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()}, {CREATE_DISK(raw, Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  Resource created = *Resources(offer.resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"))
    .begin();

  // Create a persistent MOUNT volume then launch a task to write a file.
  Resource persistentVolume = created;
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_id(id::UUID::random().toString());
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_principal(framework.principal());
  persistentVolume.mutable_disk()->mutable_volume()
    ->set_container_path("volume");
  persistentVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

  Future<Nothing> taskFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_FAILED)))
    .WillOnce(FutureSatisfy(&taskFailed));

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  // Fail resource publishing.
  Future<Nothing> nodePublishVolumeCall = futureNodePublishVolumeCall(
      &plugin, StatusError(grpc::Status(grpc::UNIMPLEMENTED, "")));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume),
       LAUNCH({createTask(
           offer.slave_id(),
           createTaskResources(persistentVolume),
           createCommandInfo("touch " + path::join("volume", "file")))})});

  AWAIT_READY(nodePublishVolumeCall);

  AWAIT_READY(taskFailed);

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Destroy the persistent volume and the MOUNT disk.
  //
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateOperationStatusMessage> destroyDiskOperationStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);
  Future<UpdateOperationStatusMessage> destroyOperationStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(raw)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()}, {DESTROY(persistentVolume), DESTROY_DISK(created)});

  AWAIT_READY(destroyOperationStatus);
  EXPECT_EQ(OPERATION_FINISHED, destroyOperationStatus->status().state());

  AWAIT_READY(destroyDiskOperationStatus);
  EXPECT_EQ(OPERATION_FINISHED, destroyDiskOperationStatus->status().state());

  AWAIT_EXPECT_READY(offers)
    << "Failed to wait for an offer containing resource '" << raw << "'";
}


// This test verifies that if a persistent volumes is never published by the
// storage local resource provider, the volume can be destroyed after recovery.
//
// To accomplish this:
//   1. Create a MOUNT disk from a RAW disk resource.
//   2. Create a persistent volume on the MOUNT disk then launches a task to
//      write a file into it.
//   3. Return `UNIMPLEMENTED` for the `NodePublishVolume` call. The task will
//      fail to launch.
//   4. Restart the agent.
//   5. Destroy the persistent volume and the MOUNT disk.
TEST_P(
    StorageLocalResourceProviderTest,
    DestroyUnpublishedPersistentVolumeWithRecovery)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  const string mockCsiEndpoint =
    "unix://" + path::join(sandbox.get(), "mock_csi.sock");

  MockCSIPlugin plugin;
  ASSERT_SOME(plugin.startup(mockCsiEndpoint));

  setupResourceProviderConfig(Bytes(0), None(), mockCsiEndpoint);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
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

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  // Create a MOUNT disk.
  Resource raw = *Resources(offer.resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()}, {CREATE_DISK(raw, Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  Resource created = *Resources(offer.resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"))
    .begin();

  // Create a persistent MOUNT volume then launch a task to write a file.
  Resource persistentVolume = created;
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_id(id::UUID::random().toString());
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_principal(framework.principal());
  persistentVolume.mutable_disk()->mutable_volume()
    ->set_container_path("volume");
  persistentVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

  Future<Nothing> taskFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_FAILED)))
    .WillOnce(FutureSatisfy(&taskFailed));

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  // Fail resource publishing.
  Future<Nothing> nodePublishVolumeCall = futureNodePublishVolumeCall(
      &plugin, StatusError(grpc::Status(grpc::UNIMPLEMENTED, "")));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume),
       LAUNCH({createTask(
           offer.slave_id(),
           createTaskResources(persistentVolume),
           createCommandInfo("touch " + path::join("volume", "file")))})});

  AWAIT_READY(nodePublishVolumeCall);

  AWAIT_READY(taskFailed);

  AWAIT_READY(offers);

  // Restart the agent.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave.get()->terminate();

  // NOTE: The mock CSI plugin always returns 4GB for `GetCapacity` calls, hence
  // the resource provider would report an extra storage pool, and when it shows
  // up, we know that the resource provider has finished reconciling storage
  // pools and thus operations won't be dropped.
  //
  // To achieve this, we drop `SlaveRegisteredMessage`s other than the first one
  // to avoid unexpected `UpdateSlaveMessage`s. Then, we also drop the first two
  // `UpdateSlaveMessage`s (one sent after agent reregistration and one after
  // resource provider reregistration) and wait for the third one, which should
  // contain the extra storage pool. We let it fall through to trigger an offer
  // allocation for the persistent volume.
  //
  // Since the extra storage pool is never used, we reject the offers if only
  // the storage pool is presented.
  //
  // TODO(chhsiao): Remove this workaround once MESOS-9553 is done.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // NOTE: The order of these expectations is reversed because Google Mock will
  // search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUFS(SlaveReregisteredMessage(), _, _);
  FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());
  ASSERT_FALSE(Resources(
      updateSlaveMessage->resource_providers().providers(0).total_resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .empty());

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Destroy the persistent volume and the MOUNT disk.
  //
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateOperationStatusMessage> destroyDiskOperationStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);
  Future<UpdateOperationStatusMessage> destroyOperationStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(raw)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()}, {DESTROY(persistentVolume), DESTROY_DISK(created)});

  AWAIT_READY(destroyOperationStatus);
  EXPECT_EQ(OPERATION_FINISHED, destroyOperationStatus->status().state());

  AWAIT_READY(destroyDiskOperationStatus);
  EXPECT_EQ(OPERATION_FINISHED, destroyDiskOperationStatus->status().state());

  AWAIT_EXPECT_READY(offers)
    << "Failed to wait for an offer containing resource '" << raw << "'";
}


// This test verifies that if a persistent volumes is never published by the
// storage local resource provider, the volume can be destroyed after agent
// reboot.
//
// To accomplish this:
//   1. Create a MOUNT disk from a RAW disk resource.
//   2. Create a persistent volume on the MOUNT disk then launches a task to
//      write a file into it.
//   3. Return `UNIMPLEMENTED` for the `NodePublishVolume` call. The task will
//      fail to launch.
//   4. Simulate an agent reboot.
//   5. Destroy the persistent volume and the MOUNT disk.
TEST_P(
    StorageLocalResourceProviderTest,
    DestroyUnpublishedPersistentVolumeWithReboot)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  const string mockCsiEndpoint =
    "unix://" + path::join(sandbox.get(), "mock_csi.sock");

  MockCSIPlugin plugin;
  ASSERT_SOME(plugin.startup(mockCsiEndpoint));

  setupResourceProviderConfig(Bytes(0), None(), mockCsiEndpoint);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
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

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  // Create a MOUNT disk.
  Resource raw = *Resources(offer.resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()}, {CREATE_DISK(raw, Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  Resource created = *Resources(offer.resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"))
    .begin();

  // Create a persistent MOUNT volume then launch a task to write a file.
  Resource persistentVolume = created;
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_id(id::UUID::random().toString());
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_principal(framework.principal());
  persistentVolume.mutable_disk()->mutable_volume()
    ->set_container_path("volume");
  persistentVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

  Future<Nothing> taskFailed;
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_FAILED)))
    .WillOnce(FutureSatisfy(&taskFailed));

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  // Fail resource publishing.
  Future<Nothing> nodePublishVolumeCall = futureNodePublishVolumeCall(
      &plugin, StatusError(grpc::Status(grpc::UNIMPLEMENTED, "")));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume),
       LAUNCH({createTask(
           offer.slave_id(),
           createTaskResources(persistentVolume),
           createCommandInfo("touch " + path::join("volume", "file")))})});

  AWAIT_READY(nodePublishVolumeCall);

  AWAIT_READY(taskFailed);

  AWAIT_READY(offers);

  // Shutdown the agent and unmount all CSI volumes to simulate a reboot.
  EXPECT_CALL(sched, offerRescinded(_, _));

  slave->reset();

  const string csiRootDir = slave::paths::getCsiRootDir(slaveFlags.work_dir);
  ASSERT_SOME(fs::unmountAll(csiRootDir));

  // Inject the boot IDs to simulate a reboot.
  ASSERT_SOME(os::write(
      slave::paths::getBootIdPath(
          slave::paths::getMetaRootDir(slaveFlags.work_dir)),
      "rebooted! ;)"));

  Try<list<string>> volumePaths =
    csi::paths::getVolumePaths(csiRootDir, "*", "*");
  ASSERT_SOME(volumePaths);
  ASSERT_FALSE(volumePaths->empty());

  foreach (const string& path, volumePaths.get()) {
    Try<csi::paths::VolumePath> volumePath =
      csi::paths::parseVolumePath(csiRootDir, path);
    ASSERT_SOME(volumePath);

    const string volumeStatePath = csi::paths::getVolumeStatePath(
        csiRootDir, volumePath->type, volumePath->name, volumePath->volumeId);

    Result<csi::state::VolumeState> volumeState =
      slave::state::read<csi::state::VolumeState>(volumeStatePath);

    ASSERT_SOME(volumeState);

    if (volumeState->state() == csi::state::VolumeState::PUBLISHED) {
      volumeState->set_boot_id("rebooted! ;)");
      ASSERT_SOME(slave::state::checkpoint(volumeStatePath, volumeState.get()));
    }
  }

  // NOTE: The mock CSI plugin always returns 4GB for `GetCapacity` calls, hence
  // the resource provider would report an extra storage pool, and when it shows
  // up, we know that the resource provider has finished reconciling storage
  // pools and thus operations won't be dropped.
  //
  // To achieve this, we drop `SlaveRegisteredMessage`s other than the first one
  // to avoid unexpected `UpdateSlaveMessage`s. Then, we also drop the first two
  // `UpdateSlaveMessage`s (one sent after agent reregistration and one after
  // resource provider reregistration) and wait for the third one, which should
  // contain the extra storage pool. We let it fall through to trigger an offer
  // allocation for the persistent volume.
  //
  // Since the extra storage pool is never used, we reject the offers if only
  // the storage pool is presented.
  //
  // TODO(chhsiao): Remove this workaround once MESOS-9553 is done.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // NOTE: The order of these expectations is reversed because Google Mock will
  // search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUFS(SlaveReregisteredMessage(), _, _);
  FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Restart the agent.
  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());
  ASSERT_FALSE(Resources(
      updateSlaveMessage->resource_providers().providers(0).total_resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .empty());

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Destroy the persistent volume and the MOUNT disk.
  //
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateOperationStatusMessage> destroyDiskOperationStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);
  Future<UpdateOperationStatusMessage> destroyOperationStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(raw)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()}, {DESTROY(persistentVolume), DESTROY_DISK(created)});

  AWAIT_READY(destroyOperationStatus);
  EXPECT_EQ(OPERATION_FINISHED, destroyOperationStatus->status().state());

  AWAIT_READY(destroyDiskOperationStatus);
  EXPECT_EQ(OPERATION_FINISHED, destroyDiskOperationStatus->status().state());

  AWAIT_EXPECT_READY(offers)
    << "Failed to wait for an offer containing resource '" << raw << "'";
}


// This test verifies that if the storage local resource provider fails to clean
// up a persistent volume, the volume will not be destroyed.
//
// To accomplish this:
//   1. Creates a MOUNT disk from a RAW disk resource.
//   2. Creates a persistent volume on the MOUNT disk then launches a task to
//      write a file into it.
//   3. Bind-mounts a file in the CSI volume so the persistent volume cleanup
//      would fail with EBUSY.
//   4. Destroys the persistent volume and the MOUNT disk. `DESTROY` would fail
//      and `DESTROY_DISK` would be dropped.
TEST_P(
    StorageLocalResourceProviderTest, ROOT_DestroyPersistentMountVolumeFailed)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
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

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers. The master can send such offers before the
  // resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  // Create a MOUNT disk.
  Resource raw = *Resources(offer.resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()}, {CREATE_DISK(raw, Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  Resource created = *Resources(offer.resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"))
    .begin();

  // Create a persistent MOUNT volume then launch a task to write a file.
  Resource persistentVolume = created;
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_id(id::UUID::random().toString());
  persistentVolume.mutable_disk()->mutable_persistence()
    ->set_principal(framework.principal());
  persistentVolume.mutable_disk()->mutable_volume()
    ->set_container_path("volume");
  persistentVolume.mutable_disk()->mutable_volume()->set_mode(Volume::RW);

  Future<Nothing> taskFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_STARTING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_RUNNING)));
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusStateEq(TASK_FINISHED)))
    .WillOnce(FutureSatisfy(&taskFinished));

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {CREATE(persistentVolume),
       LAUNCH({createTask(
           offer.slave_id(),
           createTaskResources(persistentVolume),
           createCommandInfo("touch " + path::join("volume", "file")))})});

  AWAIT_READY(taskFinished);

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Bind-mount the file in the CSI volume to fail persistent volume cleanup.
  const string& volumePath = created.disk().source().id();
  const string filePath = path::join(volumePath, "file");
  ASSERT_TRUE(os::exists(filePath));
  ASSERT_SOME(fs::mount(filePath, filePath, None(), MS_BIND, None()));

  // Destroy the persistent volume and the MOUNT disk, but none will succeed.
  //
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateOperationStatusMessage> destroyDiskOperationStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);
  Future<UpdateOperationStatusMessage> destroyOperationStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  EXPECT_CALL(
      sched, resourceOffers(&driver, OffersHaveResource(persistentVolume)))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()}, {DESTROY(persistentVolume), DESTROY_DISK(created)});

  AWAIT_READY(destroyOperationStatus);
  EXPECT_EQ(OPERATION_FAILED, destroyOperationStatus->status().state());

  AWAIT_READY(destroyDiskOperationStatus);
  EXPECT_EQ(OPERATION_DROPPED, destroyDiskOperationStatus->status().state());

  AWAIT_READY(offers);

  // Check if the MOUNT disk still exists and not being cleaned up.
  EXPECT_TRUE(os::exists(filePath));
}


// This test verifies that the storage local resource provider can import a
// preprovisioned CSI volume as a MOUNT disk of a given profile if the profile
// is known to the disk profile adaptor, and can return the space back to the
// storage pool through either destroying the MOUNT disk, or destroying a RAW
// disk directly.
TEST_P(StorageLocalResourceProviderTest, CreateDestroyPreprovisionedVolume)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  // NOTE: We set up the resource provider with an extra storage pool, so that
  // when the storage pool shows up, we know that the resource provider has
  // finished reconciling storage pools and thus operations won't be dropped.
  //
  // To achieve this, we drop `SlaveRegisteredMessage`s other than the first one
  // to avoid unexpected `UpdateSlaveMessage`s. Then, we also drop the first two
  // `UpdateSlaveMessage`s (one sent after agent registration and one after
  // resource provider registration) and wait for the third one, which should
  // contain the extra storage pool. We let it fall through to trigger an offer
  // allocation for the persistent volume.
  //
  // TODO(chhsiao): Remove this workaround once MESOS-9553 is done.
  setupResourceProviderConfig(Gigabytes(1), "volume1:2GB;volume2:2GB");

  // NOTE: The order of these expectations is reversed because Google Mock will
  // search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlaveMessage =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUF(UpdateSlaveMessage(), _, _);
  DROP_PROTOBUFS(SlaveRegisteredMessage(), _, _);
  FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(updateSlaveMessage);
  ASSERT_EQ(1, updateSlaveMessage->resource_providers().providers_size());
  ASSERT_FALSE(Resources(
      updateSlaveMessage->resource_providers().providers(0).total_resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .empty());

  // Register a framework to exercise operations.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // We use the following filter to filter offers that do not have wanted
  // resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline unwanted offers, e.g., offers containing only agent-default
  // resources and/or the extra storage pool.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  // We first wait for an offer containing the two preprovisioned volumes to
  // exercise two `CREATE_DISK` operations. Since one of them would fail, we
  // might get subsequent offers containing a preprovisioned volumes after the
  // operations are exercised, so we decline them.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      isPreprovisionedVolume<Resource>)))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(DeclineOffers(declineFilters));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  vector<Resource> preprovisioned = google::protobuf::convert<Resource>(
      Resources(offer.resources()).filter(isPreprovisionedVolume<Resource>));

  ASSERT_EQ(2u, preprovisioned.size());

  // Get the volume paths of the preprovisioned volumes.
  vector<string> volumePaths;
  foreach (const Resource& volume, preprovisioned) {
    const string& volumePath = volume.disk().source().id();
    ASSERT_TRUE(os::exists(volumePath));
    volumePaths.push_back(volumePath);
  }

  // Apply profile 'test' and 'unknown' to the preprovisioned volumes. The first
  // operation would succeed but the second would fail.
  Future<multiset<OperationState>> createDiskOperationStates =
    process::collect(
        FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _),
        FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _))
      .then([](const std::tuple<
                UpdateOperationStatusMessage,
                UpdateOperationStatusMessage>& operationStatuses) {
        return multiset<OperationState>{
          std::get<0>(operationStatuses).status().state(),
          std::get<1>(operationStatuses).status().state()};
      });

  // Once both operations are completed, we would get an offer containing both a
  // MOUNT disk and a preprovisioned volume.
  EXPECT_CALL(sched, resourceOffers(&driver, AllOf(
      OffersHaveAnyResource(
          std::bind(isMountDisk<Resource>, lambda::_1, "test")),
      OffersHaveResource(preprovisioned[1]))))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {CREATE_DISK(
           preprovisioned[0], Resource::DiskInfo::Source::MOUNT, "test"),
       CREATE_DISK(
           preprovisioned[1], Resource::DiskInfo::Source::MOUNT, "unknown")});

  multiset<OperationState> expectedOperationStates = {
    OperationState::OPERATION_FINISHED,
    OperationState::OPERATION_FAILED};

  AWAIT_EXPECT_EQ(expectedOperationStates, createDiskOperationStates);

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  Resource imported = *Resources(offer.resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"))
    .begin();

  // Destroy the imported and unimported volumes.
  Future<multiset<OperationState>> destroyDiskOperationStates =
    process::collect(
        FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _),
        FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _))
      .then([](const std::tuple<
                UpdateOperationStatusMessage,
                UpdateOperationStatusMessage>& operationStatuses) {
        return multiset<OperationState>{
          std::get<0>(operationStatuses).status().state(),
          std::get<1>(operationStatuses).status().state()};
      });

  // Once both operations are completed, we would get an offer containing a
  // storage pool that has all the freed space.
  EXPECT_CALL(sched, resourceOffers(&driver,
      OffersHaveAnyResource([](const Resource& r) {
        return isStoragePool(r, "test") &&
          Megabytes(r.scalar().value()) >= Gigabytes(4);
      })))
    .WillOnce(FutureArg<1>(&offers));

  driver.acceptOffers(
      {offer.id()},
      {DESTROY_DISK(imported),
       DESTROY_DISK(preprovisioned[1])});

  expectedOperationStates = {
    OperationState::OPERATION_FINISHED,
    OperationState::OPERATION_FINISHED};

  AWAIT_EXPECT_EQ(expectedOperationStates, destroyDiskOperationStates);

  AWAIT_READY(offers);

  // Check if the volume is deleted by the test CSI plugin.
  foreach (const string& volumePath, volumePaths) {
    EXPECT_FALSE(os::exists(volumePath));
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
TEST_P(StorageLocalResourceProviderTest, RetryOperationStatusUpdate)
{
  Clock::pause();

  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider gets subscribed after the agent
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

  // Decline offers without RAW disk resources. The master can send such offers
  // before the resource provider reports its RAW resources, or after receiving
  // the `UpdateOperationStatusMessage`.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // NOTE: If the framework has not declined an unwanted offer yet when the
  // resource provider reports its RAW resources, the new allocation triggered
  // by this update won't generate an allocatable offer due to no CPU and memory
  // resources. So we first settle the clock to ensure that the unwanted offer
  // has been declined, then advance the clock to trigger another allocation.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers->at(0);

  Option<Resource> source;
  foreach (const Resource& resource, offer.resources()) {
    if (isStoragePool(resource, "test")) {
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
      {CREATE_DISK(source.get(), Resource::DiskInfo::Source::MOUNT)});

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

  // Since the acknowledgement is delivered to the SLRP via HTTP, we wait for
  // a dispatch event to ensure that the acknowledgement is received by SLRP.
  Future<Nothing> statusUpdateManagerAcknowledgement = FUTURE_DISPATCH(
      _,
      (&StatusUpdateManagerProcess<
          id::UUID,
          UpdateOperationStatusRecord,
          UpdateOperationStatusMessage>::acknowledgement));

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);

  AWAIT_READY(retriedUpdateOperationStatusMessage);

  AWAIT_READY(acknowledgeOperationStatusMessage);
  AWAIT_READY(statusUpdateManagerAcknowledgement);

  // The master acknowledged the operation status update, so the SLRP shouldn't
  // send further operation status updates.
  EXPECT_NO_FUTURE_PROTOBUFS(UpdateOperationStatusMessage(), _, _);

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
TEST_P(
    StorageLocalResourceProviderTest,
    RetryOperationStatusUpdateAfterRecovery)
{
  Clock::pause();

  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider gets subscribed after the agent
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

  // Decline offers without RAW disk resources. The master can send such offers
  // before the resource provider reports its RAW resources, or after receiving
  // the `UpdateOperationStatusMessage`.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // NOTE: If the framework has not declined an unwanted offer yet when the
  // resource provider reports its RAW resources, the new allocation triggered
  // by this update won't generate an allocatable offer due to no CPU and memory
  // resources. So we first settle the clock to ensure that the unwanted offer
  // has been declined, then advance the clock to trigger another allocation.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers->at(0);

  Option<Resource> source;
  foreach (const Resource& resource, offer.resources()) {
    if (isStoragePool(resource, "test")) {
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
      {CREATE_DISK(source.get(), Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(droppedUpdateOperationStatusMessage);

  // Restart the agent.
  slave.get()->terminate();

  // Since the local resource provider gets subscribed after the agent
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

  // Since the acknowledgement is delivered to the SLRP via HTTP, we wait for
  // a dispatch event to ensure that the acknowledgement is received by SLRP.
  Future<Nothing> statusUpdateManagerAcknowledgement = FUTURE_DISPATCH(
      _,
      (&StatusUpdateManagerProcess<
          id::UUID,
          UpdateOperationStatusRecord,
          UpdateOperationStatusMessage>::acknowledgement));

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
  AWAIT_READY(statusUpdateManagerAcknowledgement);

  // The master has acknowledged the operation status update, so the SLRP
  // shouldn't send further operation status updates.
  EXPECT_NO_FUTURE_PROTOBUFS(UpdateOperationStatusMessage(), _, _);

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  driver.stop();
  driver.join();
}


// This test verifies that storage local resource provider properly
// reports the metric related to CSI plugin container terminations.
TEST_P(StorageLocalResourceProviderTest, ContainerTerminationMetric)
{
  // Since we want to observe a fixed number of `UpdateSlaveMessage`s,
  // register the agent with paused clock to ensure it does not
  // register multiple times due to hitting timeouts.
  Clock::pause();

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();

  slave::Fetcher fetcher(slaveFlags);

  Try<slave::MesosContainerizer*> _containerizer =
    slave::MesosContainerizer::create(slaveFlags, false, &fetcher);

  ASSERT_SOME(_containerizer);

  Owned<slave::MesosContainerizer> containerizer(_containerizer.get());

  // Since the local resource provider gets subscribed after the agent
  // is registered, it is guaranteed that the slave will send two
  // `UpdateSlaveMessage`s, where the latter one contains information from
  // the storage local resource provider.
  //
  // NOTE: The order of the two `FUTURE_PROTOBUF`s is reversed because
  // Google Mock will search the expectations in reverse order.
  Future<UpdateSlaveMessage> updateSlave2 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);
  Future<UpdateSlaveMessage> updateSlave1 =
    FUTURE_PROTOBUF(UpdateSlaveMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(
      detector.get(),
      containerizer.get(),
      slaveFlags);

  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  Clock::advance(slaveFlags.authentication_backoff_factor);

  // Wait for the the resource provider to subscribe before killing it
  // by observing the agent report the resource provider to the
  // master. This prevents the plugin from entering a failed, non-
  // restartable state, see MESOS-9130.
  //
  // TODO(bbannier): This step can be removed once MESOS-8400 is implemented.
  AWAIT_READY(updateSlave1);

  // Resume the clock so that the CSI plugin's standalone container is created
  // and the SLRP's async loop notices it.
  Clock::resume();

  AWAIT_READY(updateSlave2);
  ASSERT_FALSE(updateSlave2->resource_providers().providers().empty());

  Clock::pause();

  JSON::Object snapshot = Metrics();

  ASSERT_NE(0u, snapshot.values.count(metricName(
      "csi_plugin/container_terminations")));
  EXPECT_EQ(0, snapshot.values.at(metricName(
      "csi_plugin/container_terminations")));

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

  // Resume the clock so the plugin container is properly destroyed.
  Clock::resume();

  AWAIT_READY(pluginRestarted);

  Clock::pause();

  snapshot = Metrics();

  ASSERT_NE(0u, snapshot.values.count(metricName(
      "csi_plugin/container_terminations")));
  EXPECT_EQ(1, snapshot.values.at(metricName(
      "csi_plugin/container_terminations")));
}


// This test verifies that operation status updates contain the
// agent ID and resource provider ID of originating providers.
TEST_P(StorageLocalResourceProviderTest, OperationUpdate)
{
  Clock::pause();

  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider gets subscribed after the agent
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

  EXPECT_CALL(
      *scheduler,
      offers(_, v1::scheduler::OffersHaveAnyResource(
          std::bind(isStoragePool<v1::Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);

  const v1::FrameworkID& frameworkId = subscribed->framework_id();

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

  Option<v1::Resource> source;
  Option<mesos::v1::ResourceProviderID> resourceProviderId;
  foreach (const v1::Resource& resource, offer.resources()) {
    if (isStoragePool(resource, "test")) {
      source = resource;

      ASSERT_TRUE(resource.has_provider_id());
      resourceProviderId = resource.provider_id();

      break;
    }
  }

  ASSERT_SOME(source);
  ASSERT_SOME(resourceProviderId);

  Future<v1::scheduler::Event::UpdateOperationStatus> update;

  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&update))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  // Create a volume.
  v1::OperationID operationId;
  operationId.set_value("operation");

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {v1::CREATE_DISK(
           source.get(),
           v1::Resource::DiskInfo::Source::MOUNT,
           None(),
           operationId)}));

  AWAIT_READY(update);

  ASSERT_EQ(operationId, update->status().operation_id());
  ASSERT_EQ(
      mesos::v1::OperationState::OPERATION_FINISHED, update->status().state());
  ASSERT_TRUE(update->status().has_uuid());
  EXPECT_TRUE(metricEquals("master/operations/finished", 1));

  ASSERT_TRUE(update->status().has_agent_id());
  EXPECT_EQ(agentId, update->status().agent_id());

  ASSERT_TRUE(update->status().has_resource_provider_id());
  EXPECT_EQ(resourceProviderId.get(), update->status().resource_provider_id());
}


// This test verifies that storage local resource provider properly
// reports metrics related to operation states.
// TODO(chhsiao): Currently there is no way to test the `pending` metric for
// operations since we have no control over the completion of an operation. Once
// we support out-of-band CSI plugins through domain sockets, we could test this
// metric against a mock CSI plugin.
TEST_P(StorageLocalResourceProviderTest, OperationStateMetrics)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
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
  //   1. One containing a RAW disk resource before `CREATE_DISK`.
  //   2. One containing a MOUNT disk resource after `CREATE_DISK`.
  //   3. One containing the same MOUNT disk resource after a failed
  //      `DESTROY_DISK`.
  //
  // We set up the expectations for these offers as the test progresses.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> volumeCreatedOffers;
  Future<vector<Offer>> operationFailedOffers;

  Sequence offers;

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&rawDiskOffers));

  driver.start();

  AWAIT_READY(rawDiskOffers);
  ASSERT_FALSE(rawDiskOffers->empty());

  Option<Resource> source;

  foreach (const Resource& resource, rawDiskOffers->at(0).resources()) {
    if (isStoragePool(resource, "test")) {
      source = resource;
      break;
    }
  }

  ASSERT_SOME(source);

  JSON::Object snapshot = Metrics();

  ASSERT_NE(0u, snapshot.values.count(metricName(
      "operations/create_disk/finished")));
  EXPECT_EQ(0, snapshot.values.at(metricName(
      "operations/create_disk/finished")));
  ASSERT_NE(0u, snapshot.values.count(metricName(
      "operations/destroy_disk/failed")));
  EXPECT_EQ(0, snapshot.values.at(metricName(
      "operations/destroy_disk/failed")));
  ASSERT_NE(0u, snapshot.values.count(metricName(
      "operations/destroy_disk/dropped")));
  EXPECT_EQ(0, snapshot.values.at(metricName(
      "operations/destroy_disk/dropped")));

  // Create a volume.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeCreatedOffers));

  driver.acceptOffers(
      {rawDiskOffers->at(0).id()},
      {CREATE_DISK(source.get(), Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(volumeCreatedOffers);
  ASSERT_FALSE(volumeCreatedOffers->empty());

  Option<Resource> volume;

  foreach (const Resource& resource, volumeCreatedOffers->at(0).resources()) {
    if (isMountDisk(resource, "test")) {
      volume = resource;
      break;
    }
  }

  ASSERT_SOME(volume);
  ASSERT_TRUE(volume->disk().source().has_vendor());
  EXPECT_EQ(TEST_CSI_VENDOR, volume->disk().source().vendor());
  ASSERT_TRUE(volume->disk().source().has_id());
  ASSERT_TRUE(volume->disk().source().has_mount());
  ASSERT_TRUE(volume->disk().source().mount().has_root());
  EXPECT_FALSE(path::is_absolute(volume->disk().source().mount().root()));

  snapshot = Metrics();

  ASSERT_NE(0u, snapshot.values.count(metricName(
      "operations/create_disk/finished")));
  EXPECT_EQ(1, snapshot.values.at(metricName(
      "operations/create_disk/finished")));
  ASSERT_NE(0u, snapshot.values.count(metricName(
      "operations/destroy_disk/failed")));
  EXPECT_EQ(0, snapshot.values.at(metricName(
      "operations/destroy_disk/failed")));
  ASSERT_NE(0u, snapshot.values.count(metricName(
      "operations/destroy_disk/dropped")));
  EXPECT_EQ(0, snapshot.values.at(metricName(
      "operations/destroy_disk/dropped")));

  // Remove the volume out of band to fail `DESTROY_DISK`.
  const string& volumePath = volume->disk().source().id();
  ASSERT_SOME(os::rmdir(volumePath));

  // Destroy the created volume, which will fail.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(volume.get())))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&operationFailedOffers))
    .WillRepeatedly(DeclineOffers(declineFilters)); // Decline further offers.

  driver.acceptOffers(
      {volumeCreatedOffers->at(0).id()}, {DESTROY_DISK(volume.get())});

  AWAIT_READY(operationFailedOffers);
  ASSERT_FALSE(operationFailedOffers->empty());

  snapshot = Metrics();

  ASSERT_NE(0u, snapshot.values.count(metricName(
      "operations/create_disk/finished")));
  EXPECT_EQ(1, snapshot.values.at(metricName(
      "operations/create_disk/finished")));
  ASSERT_NE(0u, snapshot.values.count(metricName(
      "operations/destroy_disk/failed")));
  EXPECT_EQ(1, snapshot.values.at(metricName(
      "operations/destroy_disk/failed")));
  ASSERT_NE(0u, snapshot.values.count(metricName(
      "operations/destroy_disk/dropped")));
  EXPECT_EQ(0, snapshot.values.at(metricName(
      "operations/destroy_disk/dropped")));

  // Destroy the volume again, which will be dropped this time.
  Future<ApplyOperationMessage> applyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), _, _);

  driver.acceptOffers(
      {operationFailedOffers->at(0).id()}, {DESTROY_DISK(volume.get())});

  AWAIT_READY(applyOperationMessage);
  ASSERT_TRUE(applyOperationMessage
    ->resource_version_uuid().has_resource_provider_id());

  // Modify the resource version UUID to drop `DESTROY_DISK`.
  Future<UpdateOperationStatusMessage> operationDroppedStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  ApplyOperationMessage spoofedApplyOperationMessage =
    applyOperationMessage.get();
  spoofedApplyOperationMessage.mutable_resource_version_uuid()->mutable_uuid()
    ->set_value(id::UUID::random().toBytes());

  post(master.get()->pid, slave.get()->pid, spoofedApplyOperationMessage);

  AWAIT_READY(operationDroppedStatus);
  EXPECT_EQ(OPERATION_DROPPED, operationDroppedStatus->status().state());

  EXPECT_TRUE(metricEquals("master/operations/dropped", 1));
  EXPECT_TRUE(metricEquals("master/operations/create_disk/finished", 1));
  EXPECT_TRUE(metricEquals("master/operations/destroy_disk/failed", 1));
  EXPECT_TRUE(metricEquals("master/operations/destroy_disk/dropped", 1));

  EXPECT_TRUE(metricEquals(metricName(
      "operations/create_disk/finished"), 1));
  EXPECT_TRUE(metricEquals(metricName(
      "operations/destroy_disk/failed"), 1));
  EXPECT_TRUE(metricEquals(metricName(
      "operations/destroy_disk/dropped"), 1));
}


// This test verifies that storage local resource provider properly
// reports metrics related to RPCs to CSI plugins.
// TODO(chhsiao): Currently there is no way to test the `pending` and
// `cancelled` metrics for RPCs since we have no control over the completion of
// an operation. Once we support out-of-band CSI plugins through domain sockets,
// we could test these metrics against a mock CSI plugin.
TEST_P(StorageLocalResourceProviderTest, CsiPluginRpcMetrics)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
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
  //   1. One containing a RAW disk resource before `CREATE_DISK`.
  //   2. One containing a MOUNT disk resource after `CREATE_DISK`.
  //   3. One containing the same MOUNT disk resource after a failed
  //      `DESTROY_DISK`.
  //
  // We set up the expectations for these offers as the test progresses.
  Future<vector<Offer>> rawDiskOffers;
  Future<vector<Offer>> volumeCreatedOffers;
  Future<vector<Offer>> operationFailedOffers;

  Sequence offers;

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&rawDiskOffers));

  driver.start();

  AWAIT_READY(rawDiskOffers);
  ASSERT_FALSE(rawDiskOffers->empty());

  Option<Resource> source;

  foreach (const Resource& resource, rawDiskOffers->at(0).resources()) {
    if (isStoragePool(resource, "test")) {
      source = resource;
      break;
    }
  }

  ASSERT_SOME(source);

  // We expect that the following RPC calls are made during startup: `Probe`,
  // `GetPluginInfo` (2), `GetCapacity`, `GetPluginCapabilities,
  // `ControllerGetCapabilities`, `ListVolumes` (2), `NodeGetCapabilities`,
  // `NodeGetId`.
  //
  // TODO(chhsiao): As these are implementation details, we should count the
  // calls processed by a mock CSI plugin and check the metrics against that.
  const int numFinishedStartupRpcs = 10;

  EXPECT_TRUE(metricEquals(
      metricName("csi_plugin/rpcs_finished"), numFinishedStartupRpcs));
  EXPECT_TRUE(metricEquals(metricName("csi_plugin/rpcs_failed"), 0));

  // Create a volume.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&volumeCreatedOffers));

  driver.acceptOffers(
      {rawDiskOffers->at(0).id()},
      {CREATE_DISK(source.get(), Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(volumeCreatedOffers);
  ASSERT_FALSE(volumeCreatedOffers->empty());

  Option<Resource> volume;

  foreach (const Resource& resource, volumeCreatedOffers->at(0).resources()) {
    if (isMountDisk(resource, "test")) {
      volume = resource;
      break;
    }
  }

  ASSERT_SOME(volume);
  ASSERT_TRUE(volume->disk().source().has_vendor());
  EXPECT_EQ(TEST_CSI_VENDOR, volume->disk().source().vendor());
  ASSERT_TRUE(volume->disk().source().has_id());
  ASSERT_TRUE(volume->disk().source().has_mount());
  ASSERT_TRUE(volume->disk().source().mount().has_root());
  EXPECT_FALSE(path::is_absolute(volume->disk().source().mount().root()));

  // An additional `CreateVolume` RPC call is now finished.
  EXPECT_TRUE(metricEquals(
      metricName("csi_plugin/rpcs_finished"), numFinishedStartupRpcs + 1));
  EXPECT_TRUE(metricEquals(metricName("csi_plugin/rpcs_failed"), 0));

  // Remove the volume out of band to fail `DESTROY_DISK`.
  const string& volumePath = volume->disk().source().id();
  ASSERT_SOME(os::rmdir(volumePath));

  // Destroy the created volume, which will fail.
  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveResource(volume.get())))
    .InSequence(offers)
    .WillOnce(FutureArg<1>(&operationFailedOffers))
    .WillRepeatedly(DeclineOffers(declineFilters)); // Decline further offers.

  driver.acceptOffers(
      {volumeCreatedOffers->at(0).id()}, {DESTROY_DISK(volume.get())});

  AWAIT_READY(operationFailedOffers);
  ASSERT_FALSE(operationFailedOffers->empty());

  // An additional `DeleteVolume` RPC call is now failed.
  EXPECT_TRUE(metricEquals(
      metricName("csi_plugin/rpcs_finished"), numFinishedStartupRpcs + 1));
  EXPECT_TRUE(metricEquals(metricName("csi_plugin/rpcs_failed"), 1));
}


// Master reconciles operations that are missing from a reregistering slave.
// In this case, one of the two `ApplyOperationMessage`s is dropped, so the
// resource provider should send only one OPERATION_DROPPED.
//
// TODO(greggomann): Test operations on agent default resources: for such
// operations, the agent generates the dropped status.
TEST_P(StorageLocalResourceProviderTest, ReconcileDroppedOperation)
{
  Clock::pause();

  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Since the local resource provider gets subscribed after the agent is
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

  // We use the following filter to filter offers that do not have
  // wanted resources for 365 days (the maximum).
  Filters declineFilters;
  declineFilters.set_refuse_seconds(Days(365).secs());

  // Decline offers that do not contain wanted resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers(declineFilters));

  Future<vector<Offer>> offersBeforeOperations;

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offersBeforeOperations))
    .WillRepeatedly(DeclineOffers(declineFilters)); // Decline further offers.

  driver.start();

  AWAIT_READY(offersBeforeOperations);
  ASSERT_EQ(1u, offersBeforeOperations->size());

  Resources raw =
    Resources(offersBeforeOperations->at(0).resources())
      .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"));

  // Create two MOUNT disks of 2GB each.
  ASSERT_SOME_EQ(Gigabytes(4), raw.disk());
  Resource source1 = *raw.begin();
  source1.mutable_scalar()->set_value(
      static_cast<double>(Gigabytes(2).bytes()) / Bytes::MEGABYTES);
  Resource source2 = *(raw - source1).begin();

  // Drop one of the operations on the way to the agent.
  Future<ApplyOperationMessage> applyOperationMessage =
    DROP_PROTOBUF(ApplyOperationMessage(), _, _);

  // The successful operation will result in a terminal update.
  Future<UpdateOperationStatusMessage> operationFinishedStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  // Attempt the creation of two volumes.
  driver.acceptOffers(
      {offersBeforeOperations->at(0).id()},
      {CREATE_DISK(source1, Resource::DiskInfo::Source::MOUNT),
       CREATE_DISK(source2, Resource::DiskInfo::Source::MOUNT)});

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
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offersAfterOperations));

  // Advance the clock to trigger a batch allocation.
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offersAfterOperations);
  ASSERT_FALSE(offersAfterOperations->empty());

  Resources converted =
    Resources(offersAfterOperations->at(0).resources())
      .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"));

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
TEST_P(StorageLocalResourceProviderTest, RetryOperationStatusUpdateToScheduler)
{
  Clock::pause();

  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
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

  EXPECT_CALL(
      *scheduler,
      offers(_, v1::scheduler::OffersHaveAnyResource(
          std::bind(isStoragePool<v1::Resource>, lambda::_1, "test"))))
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
    if (isStoragePool(resource, "test")) {
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
  v1::OperationID operationId;
  operationId.set_value("operation");

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {v1::CREATE_DISK(
           source.get(),
           v1::Resource::DiskInfo::Source::MOUNT,
           None(),
           operationId)}));

  AWAIT_READY(update);

  ASSERT_EQ(operationId, update->status().operation_id());
  ASSERT_EQ(
      mesos::v1::OperationState::OPERATION_FINISHED, update->status().state());
  ASSERT_TRUE(update->status().has_uuid());

  ASSERT_TRUE(retriedUpdate.isPending());

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  // The scheduler didn't acknowledge the operation status update, so the SLRP
  // should resend it after the status update retry interval minimum.
  AWAIT_READY(retriedUpdate);

  ASSERT_EQ(operationId, retriedUpdate->status().operation_id());
  ASSERT_EQ(
      mesos::v1::OperationState::OPERATION_FINISHED,
      retriedUpdate->status().state());
  ASSERT_TRUE(retriedUpdate->status().has_uuid());

  // The scheduler will acknowledge the operation status update, so the agent
  // should receive an acknowledgement.
  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatusMessage =
    FUTURE_PROTOBUF(
      AcknowledgeOperationStatusMessage(), master.get()->pid, slave.get()->pid);

  // Since the acknowledgement is delivered to the SLRP via HTTP, we wait for
  // a dispatch event to ensure that the acknowledgement is received by SLRP.
  Future<Nothing> statusUpdateManagerAcknowledgement = FUTURE_DISPATCH(
      _,
      (&StatusUpdateManagerProcess<
          id::UUID,
          UpdateOperationStatusRecord,
          UpdateOperationStatusMessage>::acknowledgement));

  mesos.send(v1::createCallAcknowledgeOperationStatus(
      frameworkId, offer.agent_id(), resourceProviderId.get(), update.get()));

  AWAIT_READY(acknowledgeOperationStatusMessage);
  AWAIT_READY(statusUpdateManagerAcknowledgement);

  // Verify that the retry was only counted as one operation.
  EXPECT_TRUE(metricEquals("master/operations/finished", 1));

  // Now that the SLRP has received the acknowledgement, the SLRP shouldn't
  // send further operation status updates.
  EXPECT_NO_FUTURE_PROTOBUFS(UpdateOperationStatusMessage(), _, _);

  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();
}


// This test ensures that the master responds with the latest state
// for operations that are terminal at the master, but have not been
// acknowledged by the framework.
TEST_P(
    StorageLocalResourceProviderTest,
    ReconcileUnacknowledgedTerminalOperation)
{
  Clock::pause();

  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider gets subscribed after the agent
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

  EXPECT_CALL(
      *scheduler,
      offers(_, v1::scheduler::OffersHaveAnyResource(
          std::bind(isStoragePool<v1::Resource>, lambda::_1, "test"))))
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
    if (isStoragePool(resource, "test")) {
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
      {v1::CREATE_DISK(
           source.get(),
           v1::Resource::DiskInfo::Source::MOUNT,
           None(),
           operationId)}));

  AWAIT_READY(update);

  ASSERT_EQ(operationId, update->status().operation_id());
  ASSERT_EQ(v1::OperationState::OPERATION_FINISHED, update->status().state());
  ASSERT_TRUE(update->status().has_uuid());
  EXPECT_TRUE(metricEquals("master/operations/finished", 1));

  v1::scheduler::Call::ReconcileOperations::Operation operation;
  operation.mutable_operation_id()->CopyFrom(operationId);
  operation.mutable_agent_id()->CopyFrom(agentId);

  Future<v1::scheduler::Event::UpdateOperationStatus> reconciliationUpdate;
  EXPECT_CALL(*scheduler, updateOperationStatus(_, _))
    .WillOnce(FutureArg<1>(&reconciliationUpdate));

  const Future<v1::scheduler::APIResult> result =
    mesos.call({v1::createCallReconcileOperations(frameworkId, {operation})});

  AWAIT_READY(result);

  // The master should respond with '202 Accepted' and an
  // empty body (response field unset).
  ASSERT_EQ(process::http::Status::ACCEPTED, result->status_code());
  ASSERT_FALSE(result->has_response());

  AWAIT_READY(reconciliationUpdate);

  EXPECT_EQ(operationId, reconciliationUpdate->status().operation_id());
  EXPECT_EQ(v1::OPERATION_FINISHED, reconciliationUpdate->status().state());
  EXPECT_FALSE(reconciliationUpdate->status().has_uuid());
}


// This test verifies that the storage local resource provider will retry
// `CreateVolume` and `DeleteVolume` CSI calls with a random exponential backoff
// upon receiving `DEADLINE_EXCEEDED` or `UNAVAILABLE`.
//
// To accomplish this:
//   1. Creates a MOUNT disk from a RAW disk resource.
//   2. Returns `DEADLINE_EXCEEDED` or `UNAVAILABLE` for the first n
//      `CreateVolume` calls, where `n = numRetryableErrors` defined below. The
//      clock is advanced exponentially to trigger retries.
//   3. Returns `OK` for the next `CreateVolume` call.
//   4. Destroys the MOUNT disk.
//   5. Returns `DEADLINE_EXCEEDED` or `UNAVAILABLE` for the first n
//      `DeleteVolume` calls, where `n = numRetryableErrors` defined below. The
//      clock is advanced exponentially to trigger retries.
//   6. Returns `UNIMPLEMENTED` for the next `DeleteVolume` call to verify that
//      there is no retry on a non-retryable error.
TEST_P(StorageLocalResourceProviderTest, RetryRpcWithExponentialBackoff)
{
  Clock::pause();

  // The number of retryable errors to return for each RPC, should be >= 1.
  const size_t numRetryableErrors = 10;

  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  const string mockCsiEndpoint =
    "unix://" + path::join(sandbox.get(), "mock_csi.sock");

  MockCSIPlugin plugin;
  ASSERT_SOME(plugin.startup(mockCsiEndpoint));

  setupResourceProviderConfig(Bytes(0), None(), mockCsiEndpoint);

  master::Flags masterFlags = CreateMasterFlags();

  // Set the ping timeout to be sufficiently large to avoid agent disconnection.
  masterFlags.max_agent_ping_timeouts = 1000;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags flags = CreateSlaveFlags();
  flags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider gets subscribed after the agent
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

  // Register a framework to receive offers.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "storage");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Decline offers without RAW disk resources. The master can send such offers
  // before the resource provider receives profile updates.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers());

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Offer offer = offers->at(0);

  // We expect that the following RPC calls are made during startup: `Probe`,
  // `GetPluginInfo` (2), `GetPluginCapabilities, `ControllerGetCapabilities`,
  // `ListVolumes` (2), `GetCapacity`, `NodeGetCapabilities`, `NodeGetId`.
  //
  // TODO(chhsiao): As these are implementation details, we should count the
  // calls processed by a mock CSI plugin and check the metrics against that.
  const int numFinishedStartupRpcs = 10;

  EXPECT_TRUE(metricEquals(
      metricName("csi_plugin/rpcs_finished"), numFinishedStartupRpcs));
  EXPECT_TRUE(metricEquals(metricName("csi_plugin/rpcs_failed"), 0));

  // Create a MOUNT disk.
  Resource raw = *Resources(offer.resources())
    .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
    .begin();

  EXPECT_CALL(sched, resourceOffers(&driver, OffersHaveAnyResource(
      std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
    .WillOnce(FutureArg<1>(&offers));

  Future<UpdateOperationStatusMessage> updateOperationStatus =
    FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  // Return `DEADLINE_EXCEEDED` for the first `CreateVolume` call.
  Future<Nothing> createVolumeCall = futureCreateVolumeCall(
      &plugin, StatusError(grpc::Status(grpc::DEADLINE_EXCEEDED, "")));

  driver.acceptOffers(
      {offer.id()}, {CREATE_DISK(raw, Resource::DiskInfo::Source::MOUNT)});

  AWAIT_READY(createVolumeCall) << "Failed to wait for CreateVolume call #1";

  Duration createVolumeBackoff = csi::DEFAULT_RPC_RETRY_BACKOFF_FACTOR;

  for (size_t i = 1; i < numRetryableErrors; i++) {
    // Return `UNAVAILABLE` for subsequent `CreateVolume` calls.
    createVolumeCall = futureCreateVolumeCall(
        &plugin, StatusError(grpc::Status(grpc::UNAVAILABLE, "")));

    // Settle the clock to ensure that the retry timer has been set, then
    // advance the clock by the maximum backoff to trigger a retry.
    Clock::settle();
    Clock::advance(createVolumeBackoff);

    AWAIT_READY(createVolumeCall)
      << "Failed to wait for CreateVolume call #" << (i + 1);

    createVolumeBackoff = std::min(
        createVolumeBackoff * 2, csi::DEFAULT_RPC_RETRY_INTERVAL_MAX);
  }

  // Return a successful response for the last `CreateVolume` call.
  createVolumeCall = futureCreateVolumeCall(
      &plugin,
      csi::VolumeInfo{Megabytes(raw.scalar().value()).bytes(),
                      id::UUID::random().toString()});

  // Settle the clock to ensure that the retry timer has been set, then
  // advance the clock by the maximum backoff to trigger a retry.
  Clock::settle();
  Clock::advance(createVolumeBackoff);

  AWAIT_READY(createVolumeCall)
    << "Failed to wait for CreateVolume call #" << (numRetryableErrors + 1);

  AWAIT_READY(updateOperationStatus);
  EXPECT_EQ(OPERATION_FINISHED, updateOperationStatus->status().state());

  // Settle the clock to recover the created disk, then advance the clock to
  // trigger a batch allocation.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  offer = offers->at(0);

  // Destroy the MOUNT disk.
  Resource created = *Resources(offer.resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"))
    .begin();

  updateOperationStatus = FUTURE_PROTOBUF(UpdateOperationStatusMessage(), _, _);

  // Return `DEADLINE_EXCEEDED` for the first `DeleteVolume` call.
  Future<Nothing> deleteVolumeCall = futureDeleteVolumeCall(
      &plugin, StatusError(grpc::Status(grpc::DEADLINE_EXCEEDED, "")));

  driver.acceptOffers({offers->at(0).id()}, {DESTROY_DISK(created)});

  AWAIT_READY(deleteVolumeCall) << "Failed to wait for DeleteVolume call #1";

  Duration deleteVolumeBackoff = csi::DEFAULT_RPC_RETRY_BACKOFF_FACTOR;

  for (size_t i = 1; i < numRetryableErrors; i++) {
    // Return `UNAVAILABLE` for subsequent `DeleteVolume` calls.
    deleteVolumeCall = futureDeleteVolumeCall(
        &plugin, StatusError(grpc::Status(grpc::UNAVAILABLE, "")));

    // Settle the clock to ensure that the retry timer has been set, then
    // advance the clock by the maximum backoff to trigger a retry.
    Clock::settle();
    Clock::advance(deleteVolumeBackoff);

    AWAIT_READY(deleteVolumeCall)
      << "Failed to wait for DeleteVolume call #" << (i + 1);

    deleteVolumeBackoff = std::min(
        deleteVolumeBackoff * 2, csi::DEFAULT_RPC_RETRY_INTERVAL_MAX);
  }

  // Return a non-retryable error for the last `DeleteVolume` call.
  deleteVolumeCall = futureDeleteVolumeCall(
      &plugin, StatusError(grpc::Status(grpc::UNIMPLEMENTED, "")));

  // Settle the clock to ensure that the retry timer has been set, then
  // advance the clock by the maximum backoff to trigger a retry.
  Clock::settle();
  Clock::advance(deleteVolumeBackoff);

  AWAIT_READY(deleteVolumeCall)
    << "Failed to wait for DeleteVolume call #" << (numRetryableErrors + 1);

  AWAIT_READY(updateOperationStatus);
  EXPECT_EQ(OPERATION_FAILED, updateOperationStatus->status().state());

  // Verify that the RPC metrics count the successes and errors correctly.
  //
  // TODO(chhsiao): verify the retry metrics instead once they are in place.
  EXPECT_TRUE(metricEquals("master/operations/failed", 1));
  EXPECT_TRUE(metricEquals("master/operations/finished", 1));

  // There should be 1 finished and 10 failed `CreateVolume` calls, and 11
  // failed `DeleteVolume` calls.
  EXPECT_TRUE(metricEquals(
      metricName("csi_plugin/rpcs_finished"), numFinishedStartupRpcs + 1));
  EXPECT_TRUE(metricEquals(
      metricName("csi_plugin/rpcs_failed"), numRetryableErrors * 2 + 1));
}


// This test verifies the master can handle "non-speculative"
// (i.e. the master cannot assume the success of the operation) operation
// status updates that originate from frameworks which have been torn down.
// Non-speculative operations include CREATE_DISK.
TEST_P(
    StorageLocalResourceProviderTest,
    FrameworkTeardownBeforeTerminalOperationStatusUpdate)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to exercise an operation.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "storage");

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  // NOTE: The scheduler may connect again after the TEARDOWN call.
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo))
    .WillRepeatedly(Return());

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(v1::scheduler::DeclineOffers());

  // We are only interested in an offer with raw disk resources
  auto isRaw = [](const v1::Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().type() == v1::Resource::DiskInfo::Source::RAW;
  };

  Future<v1::scheduler::Event::Offers> offers;

  EXPECT_CALL(
      *scheduler,
      offers(_, v1::scheduler::OffersHaveAnyResource(isRaw)))
    .WillOnce(FutureArg<1>(&offers));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);

  const v1::FrameworkID& frameworkId = subscribed->framework_id();

  // Prepare to intercept the terminal operation status message between
  // agent and master, to allow the framework to teardown first.
  // This will cause the operation to become an orphan operation.
  Future<UpdateOperationStatusMessage> updateMessage =
    DROP_PROTOBUF(
        UpdateOperationStatusMessage(), slave.get()->pid, master.get()->pid);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  ASSERT_FALSE(offer.resources().empty());

  // The test relies on retry logic within the SLRP module to resend the
  // terminal operation feedback message. We pause the clock here to control
  // when this retry happens.
  Clock::pause();

  // Have the framework call CREATE_DISK, a non-speculative operation.
  Option<v1::Resource> rawDisk;

  foreach (const v1::Resource& resource, offer.resources()) {
    if (isRaw(resource)) {
      rawDisk = resource;
      break;
    }
  }

  ASSERT_SOME(rawDisk);

  v1::OperationID operationId;
  operationId.set_value("operation");

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {v1::CREATE_DISK(
          rawDisk.get(),
          v1::Resource::DiskInfo::Source::MOUNT,
          None(),
          operationId)}));

  // This message will be sent once the agent has completed the operation.
  AWAIT_READY(updateMessage);

  EXPECT_EQ(
      OperationState::OPERATION_FINISHED, updateMessage->status().state());
  EXPECT_EQ(
      operationId.value(), updateMessage->status().operation_id().value());

  // We can now tear down the framework, since we are certain the operation
  // has reached the agent.
  scheduler.reset();

  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::TEARDOWN);

    v1::master::Call::Teardown* teardown = v1Call.mutable_teardown();

    teardown->mutable_framework_id()->CopyFrom(frameworkId);

    Future<http::Response> response = process::http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(ContentType::PROTOBUF, v1Call),
        stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(http::OK().status, response);
  }

  // Since the scheduler is gone, the master will acknowledge the operation
  // status update on its behalf.
  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatusMessage =
    FUTURE_PROTOBUF(
        AcknowledgeOperationStatusMessage(),
        master.get()->pid,
        slave.get()->pid);

  // Resend the dropped operation status update.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  AWAIT_READY(acknowledgeOperationStatusMessage);

  Clock::resume();
}


// This test verifies the master will adopt orphan operations reported
// by agents upon reregistration. These orphans can appear whenever
// the master fails over, but the operation's originating framework
// never reregisters. Alternatively, and uncommonly, pending operations
// on an agent migrated from one master to another will produce orphans.
TEST_P(
    StorageLocalResourceProviderTest,
    TerminalOrphanOperationAfterMasterFailover)
{
  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath);

  setupResourceProviderConfig(Gigabytes(4));

  master::Flags masterFlags = CreateMasterFlags();

  // Greatly increase the ping timeout of the master/agent connection.
  // The clock will be advanced by `MIN_WAIT_BEFORE_ORPHAN_OPERATION_ADOPTION`
  // and if the ping timeout is less than this amount, the agent will
  // disconnect.
  masterFlags.agent_ping_timeout =
    master::DEFAULT_AGENT_PING_TIMEOUT +
    master::MIN_WAIT_BEFORE_ORPHAN_OPERATION_ADOPTION;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // NOTE: This test needs to use the StandaloneMasterDetector directly
  // (instead of the `cluster::Master::createDetector` helper) so that
  // the restarted master can be detected by the running agent, when the
  // test dictates.
  StandaloneMasterDetector detector(master.get()->pid);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Register a framework to exercise an operation.
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "storage");

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  // NOTE: The scheduler may connect again after the TEARDOWN call.
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(v1::scheduler::SendSubscribe(frameworkInfo))
    .WillRepeatedly(Return());

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  // Decline offers that contain only the agent's default resources.
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillRepeatedly(v1::scheduler::DeclineOffers());

  // We are only interested in an offer with raw disk resources
  auto isRaw = [](const v1::Resource& r) {
    return r.has_disk() &&
      r.disk().has_source() &&
      r.disk().source().has_profile() &&
      r.disk().source().type() == v1::Resource::DiskInfo::Source::RAW;
  };

  Future<v1::scheduler::Event::Offers> offers;

  EXPECT_CALL(
      *scheduler,
      offers(_, v1::scheduler::OffersHaveAnyResource(isRaw)))
    .WillOnce(FutureArg<1>(&offers));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(subscribed);

  const v1::FrameworkID& frameworkId = subscribed->framework_id();

  // Prepare to intercept the terminal operation status message between
  // agent and master, to allow the master to failover first.
  // This creates a situation where the master forgets about the
  // framework that started the operation.
  Future<UpdateOperationStatusMessage> updateMessage =
    DROP_PROTOBUF(
        UpdateOperationStatusMessage(), slave.get()->pid, master.get()->pid);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);

  ASSERT_FALSE(offer.resources().empty());

  // The test relies on retry logic within the SLRP module to resend the
  // terminal operation feedback message. We pause the clock here to control
  // when this retry happens.
  Clock::pause();

  // Have the framework call CREATE_DISK, a non-speculative operation.
  Option<v1::Resource> rawDisk;

  foreach (const v1::Resource& resource, offer.resources()) {
    if (isRaw(resource)) {
      rawDisk = resource;
      break;
    }
  }

  ASSERT_SOME(rawDisk);

  v1::OperationID operationId;
  operationId.set_value("operation");

  mesos.send(v1::createCallAccept(
      frameworkId,
      offer,
      {v1::CREATE_DISK(
          rawDisk.get(),
          v1::Resource::DiskInfo::Source::MOUNT,
          None(),
          operationId)}));

  // This message will be sent once the agent has completed the operation.
  AWAIT_READY(updateMessage);

  EXPECT_EQ(
      OperationState::OPERATION_FINISHED, updateMessage->status().state());
  EXPECT_EQ(
      operationId.value(), updateMessage->status().operation_id().value());

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Trigger a master failover to enter a situation where a terminal operation
  // is orphaned, and the originating framework is not "completed".
  detector.appoint(None());
  master->reset();

  // Get rid of the framework so that it does not reregister.
  // We want the framework to remain unknown after the master/agent recovers.
  scheduler.reset();

  // Start the master back up and wait for the agent to reregister.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);
  detector.appoint(master.get()->pid);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregisteredMessage);

  Future<AcknowledgeOperationStatusMessage> acknowledgeOperationStatusMessage =
    FUTURE_PROTOBUF(
        AcknowledgeOperationStatusMessage(),
        master.get()->pid,
        slave.get()->pid);

  // Trigger a retry of the operation status update. The master should drop
  // this one because the agent has just reregistered and the framework is
  // unknown.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  ASSERT_TRUE(acknowledgeOperationStatusMessage.isPending());

  // Trigger a retry after advancing the clock beyond the operation "adoption"
  // time, which means the master should acknowledge the terminal status.
  Clock::advance(master::MIN_WAIT_BEFORE_ORPHAN_OPERATION_ADOPTION);
  Clock::settle();

  AWAIT_READY(acknowledgeOperationStatusMessage);

  Clock::resume();
}


// This test verifies that operators can reserve/unreserve and
// create/destroy persistent volumes with resource provider resources.
TEST_P(
    StorageLocalResourceProviderTest,
    OperatorOperationsWithResourceProviderResources)
{
  Clock::pause();

  Future<shared_ptr<TestDiskProfileServer>> server =
    TestDiskProfileServer::create();

  AWAIT_READY(server);

  EXPECT_CALL(*server.get()->process, profiles(_))
    .WillOnce(Return(http::OK(createDiskProfileMapping({{"test", None()}}))))
    .WillRepeatedly(Return(Future<http::Response>())); // Stop subsequent polls.

  const Duration pollInterval = Seconds(10);
  loadUriDiskProfileAdaptorModule(
      stringify(server.get()->process->url()), pollInterval);

  setupResourceProviderConfig(Gigabytes(2));

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  // Since the local resource provider gets subscribed after the agent
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

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

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

  AWAIT_READY(slaveRegisteredMessage);

  // By default, all resource provider resources are reserved for the 'storage'
  // role. We'll refine this role to be able to add a reservation to the
  // existing one.
  const string role = string("storage/") + DEFAULT_TEST_ROLE;

  // Register a framework to create a 'MOUNT' volume and verify operator
  // operation results by checking received offers.
  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, role);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Expect some offers to be rescinded as consequence of the operator
  // operations.
  EXPECT_CALL(sched, offerRescinded(_, _))
    .WillRepeatedly(Return());

  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(DeclineOffers());

  // Create a 'MOUNT' volume from 'RAW' resources, because persistent volumes
  // are only supported for 'MOUNT' volumes.
  {
    EXPECT_CALL(
        sched,
        resourceOffers(
            &driver,
            OffersHaveAnyResource(
                std::bind(isStoragePool<Resource>, lambda::_1, "test"))))
      .WillOnce(FutureArg<1>(&offers));

    driver.start();

    Clock::advance(masterFlags.allocation_interval);

    AWAIT_READY(offers);
    ASSERT_EQ(1u, offers->size());

    const Offer offer = offers->at(0);

    Resource raw = *Resources(offer.resources())
      .filter(std::bind(isStoragePool<Resource>, lambda::_1, "test"))
      .begin();

    EXPECT_CALL(
        sched,
        resourceOffers(
            &driver,
            OffersHaveAnyResource(
                std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
      .WillOnce(FutureArg<1>(&offers));

    driver.acceptOffers(
        {offer.id()}, {CREATE_DISK(raw, Resource::DiskInfo::Source::MOUNT)});

    // NOTE: We need to resume the clock so that the resource provider can
    // periodically check if the CSI endpoint socket has been created by
    // the plugin container, which runs in another Linux process.
    Clock::resume();

    // Wait for an offer that contains the 'MOUNT' volume with default
    // reservation.
    AWAIT_READY(offers);
    ASSERT_EQ(1u, offers->size());

    Clock::pause();
  }

  Resource mountDisk = *Resources(offers->at(0).resources())
    .filter(std::bind(isMountDisk<Resource>, lambda::_1, "test"))
    .begin();
  const SlaveID slaveId = offers->at(0).slave_id();

  Resources reserved =
    Resources(mountDisk).pushReservation(createDynamicReservationInfo(
        role,
        DEFAULT_CREDENTIAL.principal()));
  reserved.unallocate();

  auto isReservedMountDisk = [](const Resource& r) {
    return
        r.has_disk() &&
        r.disk().has_source() &&
        r.disk().source().type() == Resource::DiskInfo::Source::MOUNT &&
        r.disk().source().has_vendor() &&
        r.disk().source().vendor() == TEST_CSI_VENDOR &&
        r.disk().source().has_id() &&
        r.disk().source().has_profile() &&
        !r.disk().has_persistence() &&
        r.reservations().size() == 2; // Existing and refined reservation.
  };

  // Reserve resources as operator.
  {
    v1::master::Call v1ReserveResourcesCall;
    v1ReserveResourcesCall.set_type(v1::master::Call::RESERVE_RESOURCES);

    v1::master::Call::ReserveResources* reserveResources =
      v1ReserveResourcesCall.mutable_reserve_resources();

    reserveResources->mutable_agent_id()->CopyFrom(evolve(slaveId));
    reserveResources->mutable_resources()->CopyFrom(evolve(reserved));

    EXPECT_CALL(
        sched,
        resourceOffers(&driver, OffersHaveAnyResource(isReservedMountDisk)))
      .WillOnce(FutureArg<1>(&offers));

    Future<http::Response> v1ReserveResourcesResponse = http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(ContentType::PROTOBUF, v1ReserveResourcesCall),
        stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::Accepted().status, v1ReserveResourcesResponse);

    Clock::advance(masterFlags.allocation_interval);

    // Wait for an offer that contains the 'MOUNT' volume with refined
    // reservation.
    AWAIT_READY(offers);
    ASSERT_EQ(1u, offers->size());
  }

  vector<Resource> converted;
  foreach (Resource resource, reserved) {
    if (Resources::isDisk(resource, Resource::DiskInfo::Source::MOUNT)) {
      resource.mutable_disk()->mutable_persistence()->set_id(
          id::UUID::random().toString());
      resource.mutable_disk()->mutable_persistence()->set_principal(
          DEFAULT_CREDENTIAL.principal());
      resource.mutable_disk()->mutable_volume()->set_container_path("volume");
      resource.mutable_disk()->mutable_volume()->set_mode(Volume::RW);
      converted.push_back(resource);
    }
  }

  Resources volumes(converted);

  auto isPersistedMountDisk = [](const Resource& r) {
    return
        r.has_disk() &&
        r.disk().has_source() &&
        r.disk().source().type() == Resource::DiskInfo::Source::MOUNT &&
        r.disk().source().has_vendor() &&
        r.disk().source().vendor() == TEST_CSI_VENDOR &&
        r.disk().source().has_id() &&
        r.disk().source().has_profile() &&
        r.disk().has_persistence();
  };

  // Create persistent volumes as operator.
  {
    v1::master::Call v1CreateVolumesCall;
    v1CreateVolumesCall.set_type(v1::master::Call::CREATE_VOLUMES);

    v1::master::Call::CreateVolumes* createVolumes =
      v1CreateVolumesCall.mutable_create_volumes();

    createVolumes->mutable_agent_id()->CopyFrom(evolve(slaveId));
    createVolumes->mutable_volumes()->CopyFrom(evolve(volumes));

    EXPECT_CALL(
        sched,
        resourceOffers(&driver, OffersHaveAnyResource(isPersistedMountDisk)))
      .WillOnce(FutureArg<1>(&offers));

    Future<http::Response> v1CreateVolumesResponse = http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(ContentType::PROTOBUF, v1CreateVolumesCall),
        stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::Accepted().status, v1CreateVolumesResponse);

    // Remove all offer filters. Existing filters would stop us from
    // getting offers otherwise.
    driver.reviveOffers();

    Clock::advance(masterFlags.allocation_interval);

    // Wait for an offer that contains the 'MOUNT' volume with refined
    // reservation and persistence.
    AWAIT_READY(offers);
    ASSERT_EQ(1u, offers->size());
  }

  // Destroy persistent volumes as operator.
  {
    v1::master::Call v1DestroyVolumesCall;
    v1DestroyVolumesCall.set_type(v1::master::Call::DESTROY_VOLUMES);

    v1::master::Call::DestroyVolumes* destroyVolumes =
      v1DestroyVolumesCall.mutable_destroy_volumes();

    destroyVolumes->mutable_agent_id()->CopyFrom(evolve(slaveId));
    destroyVolumes->mutable_volumes()->CopyFrom(evolve(volumes));

    EXPECT_CALL(
        sched,
        resourceOffers(&driver, OffersHaveAnyResource(isReservedMountDisk)))
      .WillOnce(FutureArg<1>(&offers));

    Future<http::Response> v1DestroyVolumesResponse = http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(ContentType::PROTOBUF, v1DestroyVolumesCall),
        stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::Accepted().status, v1DestroyVolumesResponse);

    Clock::advance(masterFlags.allocation_interval);

    // Wait for an offer that contains the 'MOUNT' volume with refined
    // reservation.
    AWAIT_READY(offers);
    ASSERT_EQ(1u, offers->size());
  }

  // Unreserve resources as operator.
  {
    v1::master::Call v1UnreserveResourcesCall;
    v1UnreserveResourcesCall.set_type(v1::master::Call::UNRESERVE_RESOURCES);
    v1::master::Call::UnreserveResources* unreserveResources =
      v1UnreserveResourcesCall.mutable_unreserve_resources();

    unreserveResources->mutable_agent_id()->CopyFrom(evolve(slaveId));
    unreserveResources->mutable_resources()->CopyFrom(evolve(reserved));

    EXPECT_CALL(
        sched,
        resourceOffers(
            &driver,
            OffersHaveAnyResource(
                std::bind(isMountDisk<Resource>, lambda::_1, "test"))))
      .WillOnce(FutureArg<1>(&offers));

    Future<http::Response> v1UnreserveResourcesResponse = http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(ContentType::PROTOBUF, v1UnreserveResourcesCall),
        stringify(ContentType::PROTOBUF));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(
        http::Accepted().status, v1UnreserveResourcesResponse);

    Clock::advance(masterFlags.allocation_interval);

    // Wait for an offer that contains the 'MOUNT' volume with default
    // reservation.
    AWAIT_READY(offers);
    ASSERT_EQ(1u, offers->size());
  }
}


// This test validates that the SLRP periodically
// reconciles resources with the CSI plugin.
TEST_P(StorageLocalResourceProviderTest, Update)
{
  Clock::pause();

  const string profilesPath = path::join(sandbox.get(), "profiles.json");

  ASSERT_SOME(
      os::write(profilesPath, createDiskProfileMapping({{"test", None()}})));

  loadUriDiskProfileAdaptorModule(profilesPath, Duration::max());

  const string mockCsiEndpoint =
    "unix://" + path::join(sandbox.get(), "mock_csi.sock");

  MockCSIPlugin plugin;
  ASSERT_SOME(plugin.startup(mockCsiEndpoint));

  constexpr Duration reconciliationInterval = Seconds(15);

  setupResourceProviderConfig(
      Bytes(0),
      None(),
      mockCsiEndpoint,
      None(),
      None(),
      reconciliationInterval);

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  process::Queue<Nothing> getCapacityCalls;
  process::Queue<Nothing> listVolumesCalls;
  if (GetParam() == csi::v0::API_VERSION) {
    EXPECT_CALL(plugin, ListVolumes(_, _, A<csi::v0::ListVolumesResponse*>()))
      .WillRepeatedly(Invoke([&](
          grpc::ServerContext* context,
          const csi::v0::ListVolumesRequest* request,
          csi::v0::ListVolumesResponse* response) {
        listVolumesCalls.put({});
        return grpc::Status::OK;
      }));

    EXPECT_CALL(plugin, GetCapacity(_, _, A<csi::v0::GetCapacityResponse*>()))
      .WillRepeatedly(Invoke([&](
          grpc::ServerContext* context,
          const csi::v0::GetCapacityRequest* request,
          csi::v0::GetCapacityResponse* response) {
        getCapacityCalls.put({});
        return grpc::Status::OK;
      }));
  } else if (GetParam() == csi::v1::API_VERSION) {
    EXPECT_CALL(plugin, ListVolumes(_, _, A<csi::v1::ListVolumesResponse*>()))
      .WillRepeatedly(Invoke([&](
          grpc::ServerContext* context,
          const csi::v1::ListVolumesRequest* request,
          csi::v1::ListVolumesResponse* response) {
        listVolumesCalls.put({});
        return grpc::Status::OK;
      }));

    EXPECT_CALL(plugin, GetCapacity(_, _, A<csi::v1::GetCapacityResponse*>()))
      .WillRepeatedly(Invoke([&](
          grpc::ServerContext* context,
          const csi::v1::GetCapacityRequest* request,
          csi::v1::GetCapacityResponse* response) {
        getCapacityCalls.put({});
        return grpc::Status::OK;
      }));
  }

  Future<Nothing> listVolumes1 = listVolumesCalls.get();
  Future<Nothing> listVolumes2 = listVolumesCalls.get();

  Future<Nothing> getCapacity1 = getCapacityCalls.get();
  Future<Nothing> getCapacity2 = getCapacityCalls.get();

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.disk_profile_adaptor = URI_DISK_PROFILE_ADAPTOR_NAME;

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Advance the clock to trigger agent registration and prevent retry.
  Clock::advance(slaveFlags.registration_backoff_factor);

  // NOTE: We need to resume the clock so that the resource provider can
  // periodically check if the CSI endpoint socket has been created by
  // the plugin container, which runs in another Linux process.
  Clock::resume();

  AWAIT_READY(listVolumes1);
  AWAIT_READY(getCapacity1);

  Clock::pause();

  // Advance the clock so the SLRP polls for volume and storage pool updates.
  Clock::settle();
  Clock::advance(reconciliationInterval);

  AWAIT_READY(listVolumes2);
  AWAIT_READY(getCapacity2);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
