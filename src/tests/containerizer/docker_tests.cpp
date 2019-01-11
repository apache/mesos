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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>
#include <stout/option.hpp>
#include <stout/gtest.hpp>

#include <stout/os/constants.hpp>

#include "docker/docker.hpp"

#include "mesos/resources.hpp"

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"

#include "tests/containerizer/docker_common.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {


static const string NAME_PREFIX = "mesos-docker";

#ifdef __WINDOWS__
static constexpr char DOCKER_MAPPED_DIR_PATH[] = "C:\\mnt\\mesos\\sandbox";
static constexpr char LIST_COMMAND[] = "dir";
#else
static constexpr char DOCKER_MAPPED_DIR_PATH[] = "/mnt/mesos/sandbox";

// Since Mesos doesn't support the `z/Z` flags for docker volumes, if you have
// SELinux on your system, regular `ls` will faill with permission denied.
// `ls -d` just lists the directory name if it exists, which is sufficient for
// these tests.
static constexpr char LIST_COMMAND[] = "ls -d";
#endif // __WINDOWS__

static constexpr char TEST_DIR_NAME[] = "test_dir";

class DockerTest : public MesosTest
{
  void SetUp() override
  {
    MesosTest::SetUp();

    Future<Nothing> pull = pullDockerImage(DOCKER_TEST_IMAGE);

    LOG_FIRST_N(WARNING, 1) << "Downloading " << string(DOCKER_TEST_IMAGE)
                            << ". This may take a while...";

    // The Windows image is ~200 MB, while the Linux image is ~2MB, so
    // hopefully this is enough time for the Windows image.
    AWAIT_READY_FOR(pull, Minutes(10));
  }

  void TearDown() override
  {
    Try<Owned<Docker>> docker = Docker::create(
        tests::flags.docker,
        tests::flags.docker_socket,
        false);

    ASSERT_SOME(docker);

    Future<vector<Docker::Container>> containers =
      docker.get()->ps(true, NAME_PREFIX);

    AWAIT_READY(containers);

    // Cleanup all mesos launched containers.
    foreach (const Docker::Container& container, containers.get()) {
      AWAIT_READY_FOR(docker.get()->rm(container.id, true), Seconds(30));
    }

    MesosTest::TearDown();
  }

protected:
  // Converts `path` to C:\`path` on Windows and /`path` on Linux.
  string fromRootDir(const string& str) {
#ifdef __WINDOWS__
    return path::join("C:", str);
#else
    return "/" + str;
#endif // __WINDOWS__
  }

  Volume createDockerVolume(
      const string& driver,
      const string& name,
      const string& containerPath)
  {
    Volume volume;
    volume.set_mode(Volume::RW);
    volume.set_container_path(containerPath);

    Volume::Source* source = volume.mutable_source();
    source->set_type(Volume::Source::DOCKER_VOLUME);

    Volume::Source::DockerVolume* docker = source->mutable_docker_volume();
    docker->set_driver(driver);
    docker->set_name(name);

    return volume;
  }
};


// This test tests the functionality of the docker's interfaces.
TEST_F(DockerTest, ROOT_DOCKER_interface)
{
  const string containerName = NAME_PREFIX + "-test";
  Resources resources = Resources::parse("cpus:1;mem:512").get();

  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  // Verify that we do not see the container.
  Future<vector<Docker::Container>> containers =
    docker->ps(true, containerName);
  AWAIT_READY(containers);

  foreach (const Docker::Container& container, containers.get()) {
    EXPECT_NE("/" + containerName, container.name);
  }

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image(DOCKER_TEST_IMAGE);

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_value(SLEEP_COMMAND(120));

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      containerName,
      directory.get(),
      DOCKER_MAPPED_DIR_PATH,
      resources);

  ASSERT_SOME(runOptions);

  // Start the container.
  Future<Option<int>> status = docker->run(runOptions.get());

  Future<Docker::Container> inspect =
    docker->inspect(containerName, Seconds(1));

  AWAIT_READY(inspect);

  // Should be able to see the container now.
  containers = docker->ps();
  AWAIT_READY(containers);
  bool found = false;
  foreach (const Docker::Container& container, containers.get()) {
    if ("/" + containerName == container.name) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);

  // Test some fields of the container.
  EXPECT_NE("", inspect->id);
  EXPECT_EQ("/" + containerName, inspect->name);
  EXPECT_SOME(inspect->pid);

  // Stop the container.
  Future<Nothing> stop = docker->stop(containerName);
  AWAIT_READY(stop);

  assertDockerKillStatus(status);

  // Now, the container should not appear in the result of ps().
  // But it should appear in the result of ps(true).
  containers = docker->ps();
  AWAIT_READY(containers);
  foreach (const Docker::Container& container, containers.get()) {
    EXPECT_NE("/" + containerName, container.name);
  }

  containers = docker->ps(true, containerName);
  AWAIT_READY(containers);
  found = false;
  foreach (const Docker::Container& container, containers.get()) {
    if ("/" + containerName == container.name) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);

  // Check the container's info, both id and name should remain the
  // same since we haven't removed it, but the pid should be none
  // since it's not running.
  inspect = docker->inspect(containerName);
  AWAIT_READY(inspect);

  EXPECT_NE("", inspect->id);
  EXPECT_EQ("/" + containerName, inspect->name);
  EXPECT_NONE(inspect->pid);

  // Remove the container.
  Future<Nothing> rm = docker->rm(containerName);
  AWAIT_READY(rm);

  // Should not be able to inspect the container.
  inspect = docker->inspect(containerName);
  AWAIT_FAILED(inspect);

  // Also, now we should not be able to see the container by invoking
  // ps(true).
  containers = docker->ps(true, containerName);
  AWAIT_READY(containers);
  foreach (const Docker::Container& container, containers.get()) {
    EXPECT_NE("/" + containerName, container.name);
  }

  runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      containerName,
      directory.get(),
      DOCKER_MAPPED_DIR_PATH,
      resources);

  ASSERT_SOME(runOptions);

  // Start the container again, this time we will do a "rm -f"
  // directly, instead of stopping and rm.
  status = docker->run(runOptions.get());

  inspect = docker->inspect(containerName, Seconds(1));
  AWAIT_READY(inspect);

  // Verify that the container is there.
  containers = docker->ps();
  AWAIT_READY(containers);
  found = false;
  foreach (const Docker::Container& container, containers.get()) {
    if ("/" + containerName == container.name) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);

  // Then do a "rm -f".
  rm = docker->rm(containerName, true);
  AWAIT_READY(rm);

  assertDockerKillStatus(status);

  // Verify that the container is totally removed, that is we can't
  // find it by ps() or ps(true).
  containers = docker->ps();
  AWAIT_READY(containers);
  foreach (const Docker::Container& container, containers.get()) {
    EXPECT_NE("/" + containerName, container.name);
  }
  containers = docker->ps(true, containerName);
  AWAIT_READY(containers);
  foreach (const Docker::Container& container, containers.get()) {
    EXPECT_NE("/" + containerName, container.name);
  }
}


// This tests our 'docker kill' wrapper.
TEST_F(DockerTest, ROOT_DOCKER_kill)
{
  const string containerName = NAME_PREFIX + "-test";
  Resources resources = Resources::parse("cpus:1;mem:512").get();

  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image(DOCKER_TEST_IMAGE);

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_value(SLEEP_COMMAND(120));

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      containerName,
      directory.get(),
      DOCKER_MAPPED_DIR_PATH,
      resources);

  ASSERT_SOME(runOptions);

  // Start the container, kill it, and expect it to terminate.
  Future<Option<int>> run = docker->run(runOptions.get());

  // Note that we cannot issue the kill until we know that the
  // run has been processed. We check for this by waiting for
  // a successful 'inspect' result.
  Future<Docker::Container> inspect =
    docker->inspect(containerName, Milliseconds(10));

  AWAIT_READY(inspect);

  Future<Nothing> kill = docker->kill(
      containerName,
      SIGKILL);

  AWAIT_READY(kill);

  assertDockerKillStatus(run);

  // Now, the container should not appear in the result of ps().
  // But it should appear in the result of ps(true).
  Future<vector<Docker::Container>> containers = docker->ps();
  AWAIT_READY(containers);

  auto nameEq = [&containerName](const Docker::Container& container) {
    return "/" + containerName == container.name;
  };

  EXPECT_TRUE(std::none_of(containers->begin(), containers->end(), nameEq));

  containers = docker->ps(true, containerName);
  AWAIT_READY(containers);
  auto ps = std::find_if(containers->begin(), containers->end(), nameEq);
  ASSERT_TRUE(ps != containers->end());

  // The container returned from ps should match the name from inspect.
  // Note that the name is different from the name in `docker ps`,
  // because `docker->ps()` internally calls `docker inspect` for each
  // container shown by `docker ps`.
  EXPECT_EQ(inspect->name, ps->name);
  EXPECT_EQ(inspect->id, ps->id);

  // Check the container's info, both id and name should remain the
  // same since we haven't removed it, but the pid should be none
  // since it's not running.
  inspect = docker->inspect(containerName);
  AWAIT_READY(inspect);

  EXPECT_EQ(ps->id, inspect->id);
  EXPECT_EQ(ps->name, inspect->name);
  EXPECT_NONE(inspect->pid);
}


// This test tests parsing docker version output.
TEST_F(DockerTest, ROOT_DOCKER_Version)
{
  Try<Owned<Docker>> docker = Docker::create(
      "echo Docker version 1.7.1, build",
      tests::flags.docker_socket,
      false);
  ASSERT_SOME(docker);

  AWAIT_EXPECT_EQ(Version(1, 7, 1), docker.get()->version());

  docker = Docker::create(
      "echo Docker version 1.7.1.fc22, build",
      tests::flags.docker_socket,
      false);
  ASSERT_SOME(docker);

  AWAIT_EXPECT_EQ(Version(1, 7, 1), docker.get()->version());

  docker = Docker::create(
      "echo Docker version 1.7.1-fc22, build",
      tests::flags.docker_socket,
      false);
  ASSERT_SOME(docker);

  AWAIT_EXPECT_EQ(Version(1, 7, 1, {"fc22"}), docker.get()->version());

  docker = Docker::create(
      "echo Docker version 17.05.0-ce, build 89658bed64",
      tests::flags.docker_socket,
      false);
  ASSERT_SOME(docker);

  AWAIT_EXPECT_EQ(Version(17, 05, 0, {"ce"}), docker.get()->version());
}


TEST_F(DockerTest, ROOT_DOCKER_CheckCommandWithShell)
{
  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image(DOCKER_TEST_IMAGE);
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_shell(true);

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      "testContainer",
      "dir",
      DOCKER_MAPPED_DIR_PATH);

  ASSERT_ERROR(runOptions);
}


TEST_F(DockerTest, ROOT_DOCKER_CheckPortResource)
{
  const string containerName = NAME_PREFIX + "-port-resource-test";

  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  // Make sure the container is removed.
  Future<Nothing> remove = docker->rm(containerName, true);

  ASSERT_TRUE(process::internal::await(remove, Seconds(10)));

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image(DOCKER_TEST_IMAGE);
  dockerInfo.set_network(ContainerInfo::DockerInfo::BRIDGE);

  ContainerInfo::DockerInfo::PortMapping portMapping;
  portMapping.set_host_port(10000);
  portMapping.set_container_port(80);

  dockerInfo.add_port_mappings()->CopyFrom(portMapping);
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
#ifdef __WINDOWS__
  commandInfo.set_shell(true);
  commandInfo.set_value("exit 0");
#else
  commandInfo.set_shell(false);
  commandInfo.set_value("true");
#endif // __WINDOWS__

  Resources resources =
    Resources::parse("ports:[9998-9999];ports:[10001-11000]").get();

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      containerName,
      "dir",
      DOCKER_MAPPED_DIR_PATH,
      resources);

  ASSERT_ERROR(runOptions);

  resources = Resources::parse("ports:[9998-9999];ports:[10000-11000]").get();

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      containerName,
      directory.get(),
      DOCKER_MAPPED_DIR_PATH,
      resources);

  ASSERT_SOME(runOptions);

  Future<Option<int>> run = docker->run(runOptions.get());

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, run);
}


TEST_F(DockerTest, ROOT_DOCKER_CancelPull)
{
  // Delete the test image if it exists.

  Try<Subprocess> s = process::subprocess(
      tests::flags.docker + " rmi lingmann/1gb",
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PATH(os::DEV_NULL));

  ASSERT_SOME(s);

  AWAIT_READY_FOR(s->status(), Seconds(30));

  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  // Assume that pulling the very large image 'lingmann/1gb' will take
  // sufficiently long that we can start it and discard (i.e., cancel
  // it) right away and the future will indeed get discarded.
  Future<Docker::Image> future =
    docker->pull(directory.get(), "lingmann/1gb");

  future.discard();

  AWAIT_DISCARDED(future);
}


// This test verifies mounting in a relative host path when running a
// docker container works.
TEST_F(DockerTest, ROOT_DOCKER_MountRelativeHostPath)
{
  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  const string containerPath = fromRootDir(path::join("tmp", TEST_DIR_NAME));
  const string command = string(LIST_COMMAND) + " " + containerPath;

  Volume* volume = containerInfo.add_volumes();
  volume->set_host_path(TEST_DIR_NAME);
  volume->set_container_path(containerPath);
  volume->set_mode(Volume::RO);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image(DOCKER_TEST_IMAGE);

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_shell(true);
  commandInfo.set_value(command);

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  const string testDir = path::join(directory.get(), TEST_DIR_NAME);
  EXPECT_SOME(os::mkdir(testDir));

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      NAME_PREFIX + "-mount-relative-host-path-test",
      directory.get(),
      DOCKER_MAPPED_DIR_PATH);

  Future<Option<int>> run = docker->run(runOptions.get());

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, run);
}


// This test verifies mounting in an absolute host path when running a
// docker container works.
TEST_F(DockerTest, ROOT_DOCKER_MountAbsoluteHostPath)
{
  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  const string testDir = path::join(directory.get(), TEST_DIR_NAME);
  EXPECT_SOME(os::mkdir(testDir));

  const string containerPath = fromRootDir(path::join("tmp", TEST_DIR_NAME));
  const string command = string(LIST_COMMAND) + " " + containerPath;

  Volume* volume = containerInfo.add_volumes();
  volume->set_host_path(testDir);
  volume->set_container_path(containerPath);
  volume->set_mode(Volume::RO);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image(DOCKER_TEST_IMAGE);

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_shell(true);
  commandInfo.set_value(command);

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      NAME_PREFIX + "-mount-absolute-host-path-test",
      directory.get(),
      DOCKER_MAPPED_DIR_PATH);

  ASSERT_SOME(runOptions);

  Future<Option<int>> run = docker->run(runOptions.get());
  AWAIT_EXPECT_WEXITSTATUS_EQ(0, run);
}


// This test verifies mounting in an absolute host path to
// a relative container path when running a docker container
// works. Windows does not support mounting volumes inside
// other volumes, so skip this test for Windows.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
  DockerTest, ROOT_DOCKER_MountRelativeContainerPath)
{
  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  const string testDir = path::join(directory.get(), TEST_DIR_NAME);
  EXPECT_SOME(os::mkdir(testDir));

  const string containerPath = path::join("tmp", TEST_DIR_NAME);
  const string command =
    string(LIST_COMMAND) + " " +
    path::join(DOCKER_MAPPED_DIR_PATH, containerPath);

  Volume* volume = containerInfo.add_volumes();
  volume->set_host_path(testDir);
  volume->set_container_path(containerPath);
  volume->set_mode(Volume::RO);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image(DOCKER_TEST_IMAGE);

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_shell(true);
  commandInfo.set_value(command);

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      NAME_PREFIX + "-mount-relative-container-path-test",
      directory.get(),
      DOCKER_MAPPED_DIR_PATH);

  ASSERT_SOME(runOptions);

  Future<Option<int>> run = docker->run(runOptions.get());

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, run);
}


// This test verifies a docker container mounting relative host
// path to a relative container path fails.
TEST_F(DockerTest, ROOT_DOCKER_MountRelativeHostPathRelativeContainerPath)
{
  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  const string containerPath = path::join("tmp", TEST_DIR_NAME);
  const string command =
    string(LIST_COMMAND) + " " +
    path::join(DOCKER_MAPPED_DIR_PATH, containerPath);

  Volume* volume = containerInfo.add_volumes();
  volume->set_host_path(TEST_DIR_NAME);
  volume->set_container_path(containerPath);
  volume->set_mode(Volume::RO);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image(DOCKER_TEST_IMAGE);

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_shell(true);
  commandInfo.set_value(command);

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  const string testDir = path::join(directory.get(), TEST_DIR_NAME);
  EXPECT_SOME(os::mkdir(testDir));

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      NAME_PREFIX + "-mount-relative-host-path/container-path-test",
      directory.get(),
      DOCKER_MAPPED_DIR_PATH);

  ASSERT_ERROR(runOptions);
}


class DockerImageTest : public MesosTest {};


// This test verifies that docker image constructor is able to read
// entrypoint and environment from a docker inspect JSON object.
TEST_F(DockerImageTest, ParseInspectonImage)
{
  JSON::Value inspect = JSON::parse(
    "{"
    "    \"Id\": "
    "\"0a8ee093d995e48aa8af626b8a4c48fe3949e474b0ccca9be9d5cf08abd9eda1\","
    "    \"Parent\": "
    "\"6fbfa9a156a7655f1bbc2b3ca3624850d373fa403555ae42ed05fe5b478588fa\","
    "    \"Comment\": \"\","
    "    \"Created\": \"2015-10-01T13:24:42.549270714Z\","
    "    \"Container\": "
    "\"d87a718e07e151623b0310d82b27d2f0acdb1376755ce4aea7a26313cdab379a\","
    "    \"ContainerConfig\": {"
    "        \"Hostname\": \"7b840bf4fc5e\","
    "        \"Domainname\": \"\","
    "        \"User\": \"\","
    "        \"AttachStdin\": false,"
    "        \"AttachStdout\": false,"
    "        \"AttachStderr\": false,"
    "        \"PortSpecs\": null,"
    "        \"ExposedPorts\": null,"
    "        \"Tty\": false,"
    "        \"OpenStdin\": false,"
    "        \"StdinOnce\": false,"
    "        \"Env\": ["
    "            \"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:"
    "/sbin:/bin\","
    "            \"LANG=C.UTF-8\","
    "            \"JAVA_VERSION=8u66\","
    "            \"JAVA_DEBIAN_VERSION=8u66-b01-1~bpo8+1\","
    "            \"SPARK_OPTS=--driver-java-options=-Xms1024M --driver-java-options=-Xmx4096M --driver-java-options=-Dlog4j.logLevel=info\"," // NOLINT(whitespace/line_length)
    "            \"CA_CERTIFICATES_JAVA_VERSION=20140324\""
    "        ],"
    "        \"Cmd\": ["
    "            \"/bin/sh\","
    "            \"-c\","
    "            \"#(nop) ENTRYPOINT \\u0026{[\\\"./bin/start\\\"]}\""
    "        ],"
    "        \"Image\": "
    "\"6fbfa9a156a7655f1bbc2b3ca3624850d373fa403555ae42ed05fe5b478588fa\","
    "        \"Volumes\": null,"
    "        \"VolumeDriver\": \"\","
    "        \"WorkingDir\": \"/marathon\","
    "        \"Entrypoint\": ["
    "            \"./bin/start\""
    "        ],"
    "        \"NetworkDisabled\": false,"
    "        \"MacAddress\": \"\","
    "        \"OnBuild\": [],"
    "        \"Labels\": {}"
    "    },"
    "    \"DockerVersion\": \"1.8.3-rc1\","
    "    \"Author\": \"\","
    "    \"Config\": {"
    "        \"Hostname\": \"7b840bf4fc5e\","
    "        \"Domainname\": \"\","
    "        \"User\": \"\","
    "        \"AttachStdin\": false,"
    "        \"AttachStdout\": false,"
    "        \"AttachStderr\": false,"
    "        \"PortSpecs\": null,"
    "        \"ExposedPorts\": null,"
    "        \"Tty\": false,"
    "        \"OpenStdin\": false,"
    "        \"StdinOnce\": false,"
    "        \"Env\": ["
    "            \"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:"
    "/sbin:/bin\","
    "            \"LANG=C.UTF-8\","
    "            \"JAVA_VERSION=8u66\","
    "            \"JAVA_DEBIAN_VERSION=8u66-b01-1~bpo8+1\","
    "            \"SPARK_OPTS=--driver-java-options=-Xms1024M --driver-java-options=-Xmx4096M --driver-java-options=-Dlog4j.logLevel=info\"," // NOLINT(whitespace/line_length)
    "            \"CA_CERTIFICATES_JAVA_VERSION=20140324\""
    "        ],"
    "        \"Cmd\": null,"
    "        \"Image\": "
    "\"6fbfa9a156a7655f1bbc2b3ca3624850d373fa403555ae42ed05fe5b478588fa\","
    "        \"Volumes\": null,"
    "        \"VolumeDriver\": \"\","
    "        \"WorkingDir\": \"/marathon\","
    "        \"Entrypoint\": ["
    "            \"./bin/start\""
    "        ],"
    "        \"NetworkDisabled\": false,"
    "        \"MacAddress\": \"\","
    "        \"OnBuild\": [],"
    "        \"Labels\": {}"
    "    },"
    "    \"Architecture\": \"amd64\","
    "    \"Os\": \"linux\","
    "    \"Size\": 0,"
    "    \"VirtualSize\": 977664708"
    "}").get();

  Try<JSON::Object> json = JSON::parse<JSON::Object>(stringify(inspect));
  ASSERT_SOME(json);

  Try<Docker::Image> image = Docker::Image::create(json.get());
  ASSERT_SOME(image);

  EXPECT_EQ("./bin/start", image->entrypoint->front());
  EXPECT_EQ("C.UTF-8", image->environment->at("LANG"));
  EXPECT_EQ("8u66", image->environment->at("JAVA_VERSION"));
  EXPECT_EQ("8u66-b01-1~bpo8+1",
            image->environment->at("JAVA_DEBIAN_VERSION"));
  EXPECT_EQ("--driver-java-options=-Xms1024M "
            "--driver-java-options=-Xmx4096M "
            "--driver-java-options=-Dlog4j.logLevel=info",
            image->environment->at("SPARK_OPTS"));
  EXPECT_EQ("20140324",
            image->environment->at("CA_CERTIFICATES_JAVA_VERSION"));
}


// Tests the --devices flag of 'docker run' by adding the
// /dev/nvidiactl device (present alongside Nvidia GPUs).
// Skip this test on Windows, since GPU support does not work yet.
//
// TODO(bmahler): Avoid needing Nvidia GPUs to test this.
TEST_F_TEMP_DISABLED_ON_WINDOWS(DockerTest, ROOT_DOCKER_NVIDIA_GPU_DeviceAllow)
{
  const string containerName = NAME_PREFIX + "-test";
  Resources resources = Resources::parse("cpus:1;mem:512;gpus:1").get();

  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  // Make sure the additional device is exposed (/dev/nvidiactl) and
  // make sure that the default devices (e.g. /dev/null) are still
  // accessible.
  CommandInfo commandInfo;
  commandInfo.set_value("touch /dev/nvidiactl && touch /dev/null");

  Docker::Device nvidiaCtl;
  nvidiaCtl.hostPath = Path("/dev/nvidiactl");
  nvidiaCtl.containerPath = Path("/dev/nvidiactl");
  nvidiaCtl.access.read = true;
  nvidiaCtl.access.write = true;
  nvidiaCtl.access.mknod = true;

  vector<Docker::Device> devices = { nvidiaCtl };

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      containerName,
      directory.get(),
      "/mnt/mesos/sandbox",
      resources,
      false,
      None(),
      devices);

  ASSERT_SOME(runOptions);

  Future<Option<int>> status = docker->run(runOptions.get());

  AWAIT_EXPECT_WEXITSTATUS_EQ(0, status);
}


// Tests that devices are parsed correctly from 'docker inspect'.
// Skip this test on Windows, since GPU support does not work yet.
//
// TODO(bmahler): Avoid needing Nvidia GPUs to test this and
// merge this into a more general inspect test.
TEST_F_TEMP_DISABLED_ON_WINDOWS(
  DockerTest, ROOT_DOCKER_NVIDIA_GPU_InspectDevices)
{
  const string containerName = NAME_PREFIX + "-test";
  Resources resources = Resources::parse("cpus:1;mem:512;gpus:1").get();

  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  // Make sure the additional device is exposed (/dev/nvidiactl) and
  // make sure that the default devices (e.g. /dev/null) are still
  // accessible. We then sleep to allow time to inspect and verify
  // that the device is correctly parsed from the json.
  CommandInfo commandInfo;
  commandInfo.set_value("touch /dev/nvidiactl && touch /dev/null && sleep 120");

  Docker::Device nvidiaCtl;
  nvidiaCtl.hostPath = Path("/dev/nvidiactl");
  nvidiaCtl.containerPath = Path("/dev/nvidiactl");
  nvidiaCtl.access.read = true;
  nvidiaCtl.access.write = true;
  nvidiaCtl.access.mknod = false;

  vector<Docker::Device> devices = { nvidiaCtl };

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      containerName,
      directory.get(),
      "/mnt/mesos/sandbox",
      resources,
      false,
      None(),
      devices);

  ASSERT_SOME(runOptions);

  Future<Option<int>> status = docker->run(runOptions.get());

  Future<Docker::Container> container =
    docker->inspect(containerName, Milliseconds(1));

  AWAIT_READY(container);

  EXPECT_EQ(1u, container->devices.size());
  EXPECT_EQ(nvidiaCtl.hostPath, container->devices[0].hostPath);
  EXPECT_EQ(nvidiaCtl.containerPath, container->devices[0].hostPath);
  EXPECT_EQ(nvidiaCtl.access.read, container->devices[0].access.read);
  EXPECT_EQ(nvidiaCtl.access.write, container->devices[0].access.write);
  EXPECT_EQ(nvidiaCtl.access.mknod, container->devices[0].access.mknod);

  AWAIT_READY(docker->kill(containerName, SIGKILL));

  AWAIT_EXPECT_WEXITSTATUS_EQ(128 + SIGKILL, status);
}


// This tests verifies that a task requiring more than one volume driver (in
// multiple Volumes) is rejected.
TEST_F(DockerTest, ROOT_DOCKER_ConflictingVolumeDriversInMultipleVolumes)
{
  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  const string containerPath1 = fromRootDir(path::join("tmp", "test1"));
  const string containerPath2 = fromRootDir(path::join("tmp", "test2"));

  Volume volume1 = createDockerVolume("driver1", "name1", containerPath1);
  containerInfo.add_volumes()->CopyFrom(volume1);

  Volume volume2 = createDockerVolume("driver2", "name2", containerPath2);
  containerInfo.add_volumes()->CopyFrom(volume2);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image(DOCKER_TEST_IMAGE);
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_shell(false);

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      "testContainer",
      "dir",
      DOCKER_MAPPED_DIR_PATH);

  ASSERT_ERROR(runOptions);
}


// This tests verifies that a task requiring more than one volume driver (via
// Volume.Source.DockerInfo.driver and ContainerInfo.DockerInfo.volume_driver)
// is rejected.
TEST_F(DockerTest, ROOT_DOCKER_ConflictingVolumeDrivers)
{
  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  const string containerPath = fromRootDir(path::join("tmp", "test1"));

  Volume volume1 = createDockerVolume("driver", "name1", containerPath);
  containerInfo.add_volumes()->CopyFrom(volume1);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image(DOCKER_TEST_IMAGE);
  dockerInfo.set_volume_driver("driver1");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_shell(false);

  Try<Docker::RunOptions> runOptions = Docker::RunOptions::create(
      containerInfo,
      commandInfo,
      "testContainer",
      "dir",
      DOCKER_MAPPED_DIR_PATH);

  ASSERT_ERROR(runOptions);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
