/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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

using namespace process;

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace tests {


// This test tests the functionality of the docker's interfaces.
TEST(DockerTest, ROOT_DOCKER_interface)
{
  string containerName = "mesos-docker-test";
  Resources resources = Resources::parse("cpus:1;mem:512").get();

  Owned<Docker> docker(Docker::create(tests::flags.docker, false).get());

  // Cleaning up the container first if it exists.
  Future<Nothing> status = docker->rm(containerName, true);
  ASSERT_TRUE(status.await(Seconds(10)));

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
  dockerInfo.set_image("busybox");
  containerInfo.mutable_docker()->CopyFrom(dockerInfo);

  CommandInfo commandInfo;
  commandInfo.set_value("sleep 120");

  // Start the container.
  status = docker->run(
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


TEST(DockerTest, ROOT_DOCKER_CheckCommandWithShell)
{
  Owned<Docker> docker(Docker::create(tests::flags.docker, false).get());

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
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


TEST(DockerTest, ROOT_DOCKER_CheckPortResource)
{
  string containerName = "mesos-docker-port-resource-test";
  Owned<Docker> docker(Docker::create(tests::flags.docker, false).get());

  // Make sure the container is removed.
  Future<Nothing> remove = docker->rm(containerName, true);

  ASSERT_TRUE(process::internal::await(remove, Seconds(10)));

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::DOCKER);

  ContainerInfo::DockerInfo dockerInfo;
  dockerInfo.set_image("busybox");
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

  Future<Nothing> status = docker->rm(containerName, true);
  ASSERT_TRUE(process::internal::await(status, Seconds(10)));
}


TEST(DockerTest, ROOT_DOCKER_CancelPull)
{
  // Delete the test image if it exists.

  Try<Subprocess> s = process::subprocess(
      tests::flags.docker + " rmi lingmann/1gb",
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"));

  ASSERT_SOME(s);

  AWAIT_READY_FOR(s.get().status(), Seconds(30));

  Owned<Docker> docker(Docker::create(tests::flags.docker, false).get());

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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
