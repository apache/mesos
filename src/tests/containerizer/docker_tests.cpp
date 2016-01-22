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

#include <gtest/gtest.h>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>
#include <stout/option.hpp>
#include <stout/gtest.hpp>

#include "docker/docker.hpp"

#include "mesos/resources.hpp"

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/mesos.hpp"

using namespace process;

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace tests {


static const string NAME_PREFIX="mesos-docker";


class DockerTest : public MesosTest
{
  virtual void TearDown()
  {
    Try<Owned<Docker>> docker = Docker::create(
        tests::flags.docker,
        tests::flags.docker_socket,
        false);

    ASSERT_SOME(docker);

    Future<list<Docker::Container>> containers =
      docker.get()->ps(true, NAME_PREFIX);

    AWAIT_READY(containers);

    // Cleanup all mesos launched containers.
    foreach (const Docker::Container& container, containers.get()) {
      AWAIT_READY_FOR(docker.get()->rm(container.id, true), Seconds(30));
    }
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
  Future<list<Docker::Container> > containers = docker->ps(true, containerName);
  AWAIT_READY(containers);
  foreach (const Docker::Container& container, containers.get()) {
    EXPECT_NE("/" + containerName, container.name);
  }

  Try<string> directory = environment->mkdtemp();
  CHECK_SOME(directory) << "Failed to create temporary directory";

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_value("sleep 120");

  // Start the container.
  Future<Nothing> status = docker->run(
      containerInfo,
      commandInfo,
      containerName,
      directory.get(),
      "/mnt/mesos/sandbox",
      resources);

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
  EXPECT_NE("", inspect.get().id);
  EXPECT_EQ("/" + containerName, inspect.get().name);
  EXPECT_SOME(inspect.get().pid);

  // Stop the container.
  status = docker->stop(containerName);
  AWAIT_READY(status);

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

  EXPECT_NE("", inspect.get().id);
  EXPECT_EQ("/" + containerName, inspect.get().name);
  EXPECT_NONE(inspect.get().pid);

  // Remove the container.
  status = docker->rm(containerName);
  AWAIT_READY(status);

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

  // Start the container again, this time we will do a "rm -f"
  // directly, instead of stopping and rm.
  status = docker->run(
      containerInfo,
      commandInfo,
      containerName,
      directory.get(),
      "/mnt/mesos/sandbox",
      resources);

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
  status = docker->rm(containerName, true);
  AWAIT_READY(status);

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


// This test tests parsing docker version output.
TEST_F(DockerTest, ROOT_DOCKER_parsing_version)
{
  Owned<Docker> docker1 = Docker::create(
      "echo Docker version 1.7.1, build",
      tests::flags.docker_socket,
      false).get();

  Future<Version> version1 = docker1->version();
  AWAIT_READY(version1);
  EXPECT_EQ(version1.get(), Version::parse("1.7.1").get());

  Owned<Docker> docker2 = Docker::create(
      "echo Docker version 1.7.1.fc22, build",
      tests::flags.docker_socket,
      false).get();

  Future<Version> version2 = docker2->version();
  AWAIT_READY(version2);
  EXPECT_EQ(version2.get(), Version::parse("1.7.1").get());

  Owned<Docker> docker3 = Docker::create(
      "echo Docker version 1.7.1-fc22, build",
      tests::flags.docker_socket,
      false).get();

  Future<Version> version3 = docker3->version();
  AWAIT_READY(version3);
  EXPECT_EQ(version3.get(), Version::parse("1.7.1").get());
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
  dockerInfo.set_image("alpine");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_shell(true);

  Future<Nothing> run = docker->run(
      containerInfo,
      commandInfo,
      "testContainer",
      "dir",
      "/mnt/mesos/sandbox");

  ASSERT_TRUE(run.isFailed());
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
  dockerInfo.set_image("alpine");
  dockerInfo.set_network(ContainerInfo::DockerInfo::BRIDGE);

  ContainerInfo::DockerInfo::PortMapping portMapping;
  portMapping.set_host_port(10000);
  portMapping.set_container_port(80);

  dockerInfo.add_port_mappings()->CopyFrom(portMapping);
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_shell(false);
  commandInfo.set_value("true");

  Resources resources =
    Resources::parse("ports:[9998-9999];ports:[10001-11000]").get();

  Future<Nothing> run = docker->run(
      containerInfo,
      commandInfo,
      containerName,
      "dir",
      "/mnt/mesos/sandbox",
      resources);

  // Port should be out side of the provided ranges.
  AWAIT_EXPECT_FAILED(run);

  resources = Resources::parse("ports:[9998-9999];ports:[10000-11000]").get();

  Try<string> directory = environment->mkdtemp();
  CHECK_SOME(directory) << "Failed to create temporary directory";

  run = docker->run(
      containerInfo,
      commandInfo,
      containerName,
      directory.get(),
      "/mnt/mesos/sandbox",
      resources);

  AWAIT_READY(run);
}


TEST_F(DockerTest, ROOT_DOCKER_CancelPull)
{
  // Delete the test image if it exists.

  Try<Subprocess> s = process::subprocess(
      tests::flags.docker + " rmi lingmann/1gb",
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"));

  ASSERT_SOME(s);

  AWAIT_READY_FOR(s.get().status(), Seconds(30));

  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  Try<string> directory = environment->mkdtemp();

  CHECK_SOME(directory) << "Failed to create temporary directory";

  // Assume that pulling the very large image 'lingmann/1gb' will take
  // sufficiently long that we can start it and discard (i.e., cancel
  // it) right away and the future will indeed get discarded.
  Future<Docker::Image> future =
    docker->pull(directory.get(), "lingmann/1gb");

  future.discard();

  AWAIT_DISCARDED(future);
}


// This test verifies mounting in a relative path when running a
// docker container works.
TEST_F(DockerTest, ROOT_DOCKER_MountRelative)
{
  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  Volume* volume = containerInfo.add_volumes();
  volume->set_host_path("test_file");
  volume->set_container_path("/tmp/test_file");
  volume->set_mode(Volume::RO);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_shell(true);
  commandInfo.set_value("ls /tmp/test_file");

  Try<string> directory = environment->mkdtemp();
  CHECK_SOME(directory) << "Failed to create temporary directory";

  const string testFile = path::join(directory.get(), "test_file");
  EXPECT_SOME(os::write(testFile, "data"));

  Future<Nothing> run = docker->run(
      containerInfo,
      commandInfo,
      NAME_PREFIX + "-mount-relative-test",
      directory.get(),
      directory.get());

  AWAIT_READY(run);
}


// This test verifies mounting in an absolute path when running a
// docker container works.
TEST_F(DockerTest, ROOT_DOCKER_MountAbsolute)
{
  Owned<Docker> docker = Docker::create(
      tests::flags.docker,
      tests::flags.docker_socket,
      false).get();

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  Try<string> directory = environment->mkdtemp();
  CHECK_SOME(directory) << "Failed to create temporary directory";

  const string testFile = path::join(directory.get(), "test_file");
  EXPECT_SOME(os::write(testFile, "data"));

  Volume* volume = containerInfo.add_volumes();
  volume->set_host_path(testFile);
  volume->set_container_path("/tmp/test_file");
  volume->set_mode(Volume::RO);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("alpine");

  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_shell(true);
  commandInfo.set_value("ls /tmp/test_file");

  Future<Nothing> run = docker->run(
      containerInfo,
      commandInfo,
      NAME_PREFIX + "-mount-absolute-test",
      directory.get(),
      directory.get());

  AWAIT_READY(run);
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

  EXPECT_EQ("./bin/start", image.get().entrypoint.get().front());
  EXPECT_EQ("C.UTF-8", image.get().environment.get().at("LANG"));
  EXPECT_EQ("8u66", image.get().environment.get().at("JAVA_VERSION"));
  EXPECT_EQ("8u66-b01-1~bpo8+1",
            image.get().environment.get().at("JAVA_DEBIAN_VERSION"));
  EXPECT_EQ("20140324",
            image.get().environment.get().at("CA_CERTIFICATES_JAVA_VERSION"));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
