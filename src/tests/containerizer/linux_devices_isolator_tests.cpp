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

#include <map>
#include <ostream>
#include <string>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include "common/parse.hpp"

#include "tests/cluster.hpp"
#include "tests/mesos.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/containerizer/docker_archive.hpp"

using process::Future;
using process::Owned;

using std::map;
using std::ostream;
using std::string;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::slave::ContainerTermination;

namespace mesos {
namespace internal {
namespace tests {

struct DevicesTestParam
{
  const string containerCheck;
  const string allowedDevices;
};


ostream& operator<<(ostream& stream, const DevicesTestParam& param)
{
  return stream << param.containerCheck;
}


class LinuxDevicesIsolatorTest
  : public MesosTest,
    public ::testing::WithParamInterface<DevicesTestParam>
{
public:
  LinuxDevicesIsolatorTest()
    : param(GetParam()) {}

  DevicesTestParam param;
};


TEST_P(LinuxDevicesIsolatorTest,
       ROOT_UNPRIVILEGED_USER_PopulateWhitelistedDevices)
{
  // Verify that all the necessary devices are present on the host.
  // All reasonable Linux configuration should have these devices.
  ASSERT_TRUE(os::exists("/dev/kmsg"));
  ASSERT_TRUE(os::exists("/dev/loop-control"));
  ASSERT_TRUE(os::exists("/dev/net/tun"));

  slave::Flags flags = CreateSlaveFlags();

  flags.isolation = "linux/devices,filesystem/linux,docker/runtime";
  flags.docker_registry =  path::join(sandbox.get(), "registry");
  flags.docker_store_dir = path::join(sandbox.get(), "store");
  flags.image_providers = "docker";

  flags.allowed_devices =
    flags::parse<DeviceWhitelist>(param.allowedDevices).get();

  AWAIT_READY(DockerArchive::create(flags.docker_registry, "test_image"));

  Fetcher fetcher(flags);

  Try<MesosContainerizer*> create =
    MesosContainerizer::create(flags, true, &fetcher);

  ASSERT_SOME(create);

  Owned<Containerizer> containerizer(create.get());

  ContainerID containerId;
  containerId.set_value(id::UUID::random().toString());

  ExecutorInfo executor = createExecutorInfo(
      "test_executor",
      strings::join(";", "ls -l /dev", param.containerCheck));

  executor.mutable_container()->CopyFrom(createContainerInfo("test_image"));

  string directory = path::join(flags.work_dir, "sandbox");
  ASSERT_SOME(os::mkdir(directory));

  // Launch the container check command as the non-root user. All the
  // check commands are testing for device file access, but root will
  // always have access.
  Future<Containerizer::LaunchResult> launch = containerizer->launch(
      containerId,
      createContainerConfig(
          None(), executor, directory, os::getenv("SUDO_USER").get()),
      map<string, string>(),
      None());

  AWAIT_ASSERT_EQ(Containerizer::LaunchResult::SUCCESS, launch);

  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(wait->get().has_status());
  EXPECT_WEXITSTATUS_EQ(0, wait->get().status());
}


static const std::vector<DevicesTestParam> devicesTestValues {
  // Test that /dev/loop-control is a character device and that
  // /dev/kmsg doesn't exist. The latter test ensures that we
  // won't succeed by accidentally running on the host.
  DevicesTestParam{
    "test -c /dev/loop-control && test ! -e /dev/kmsg",
    R"~({
      "allowed_devices": [
        {
          "device": {
            "path": "/dev/loop-control"
          },
          "access": {}
        }
      ]
    })~"},
  // Test that a device in a subdirectory is populated.
  DevicesTestParam{
    "test -r /dev/net/tun",
    R"~({
      "allowed_devices": [
        {
          "device": {
            "path": "/dev/net/tun"
          },
          "access": {
            "read": true
          }
        }
      ]
    })~"},
  // Test that read-only devices are populated in read-only mode.
  DevicesTestParam{
    "test -r /dev/loop-control && test ! -w /dev/loop-control",
    R"~({
      "allowed_devices": [
        {
          "device": {
            "path": "/dev/loop-control"
          },
          "access": {
            "read": true
          }
        }
      ]
    })~"},
  // Test that write-only devices are populated in write-only mode.
  DevicesTestParam{
    "test -w /dev/loop-control && test ! -r /dev/loop-control",
    R"~({
      "allowed_devices": [
        {
          "device": {
            "path": "/dev/loop-control"
          },
          "access": {
            "write": true
          }
        }
      ]
    })~"},
  // Test that read-write devices are populated in read-write mode.
  DevicesTestParam{
    "test -w /dev/loop-control && test -r /dev/loop-control",
    R"~({
      "allowed_devices": [
        {
          "device": {
            "path": "/dev/loop-control"
          },
          "access": {
            "read": true,
            "write": true
          }
        }
      ]
    })~"}
};


INSTANTIATE_TEST_CASE_P(
  DevicesTestParam,
  LinuxDevicesIsolatorTest,
  ::testing::ValuesIn(devicesTestValues));

} // namespace tests {
} // namespace internal {
} // namespace mesos {
