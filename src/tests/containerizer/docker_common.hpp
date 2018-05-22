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

#ifndef __TEST_DOCKER_COMMON_HPP__
#define __TEST_DOCKER_COMMON_HPP__

#include <string>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/format.hpp>
#include <stout/gtest.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <stout/os/mkdtemp.hpp>

#include "docker/docker.hpp"

#include "tests/flags.hpp"

namespace mesos {
namespace internal {
namespace tests {

#ifdef __WINDOWS__
// The following image is the microsoft/nanoserver image with
// ContainerAdministrator as the default user. There are some permission bugs
// with accessing volume mounts in process (but not Hyper-V) isolation as
// the regular ContainerUser user, but accesing them as ContainerAdministrator
// works fine.
static constexpr char DOCKER_TEST_IMAGE[] = "akagup/nano-admin";

// We use a custom HTTP(S) server here, because the official `microsoft/iis`
// HTTP server image is ~20x larger than this one, so pulling it takes too
// long. This server supports HTTP and HTTPS listening on port 80 and 443.
// For more information, see https://github.com/gupta-ak/https-server.
static constexpr char DOCKER_HTTP_IMAGE[] = "akagup/https-server";
static constexpr char DOCKER_HTTP_COMMAND[] = "http.exe";
static constexpr char DOCKER_HTTPS_IMAGE[] = "akagup/https-server";
static constexpr char DOCKER_HTTPS_COMMAND[] = "http.exe";
#else
static constexpr char DOCKER_TEST_IMAGE[] = "alpine";

// The HTTP server is netcat running on alpine.
static constexpr char DOCKER_HTTP_IMAGE[] = "alpine";
static constexpr char DOCKER_HTTP_COMMAND[] =
  "nc -lk -p 80 -e echo -e \"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\"";

// Refer to https://github.com/qianzhangxa/https-server for
// how the Docker image `zhq527725/https-server` works.
static constexpr char DOCKER_HTTPS_IMAGE[] = "zhq527725/https-server";
static constexpr char DOCKER_HTTPS_COMMAND[] = "python https_server.py 443";
#endif // __WINDOWS__

constexpr char DOCKER_IPv6_NETWORK[] = "mesos-docker-ip6-test";


inline process::Future<Nothing> pullDockerImage(const std::string& imageName)
{
  Try<process::Owned<Docker>> docker =
    Docker::create(tests::flags.docker, tests::flags.docker_socket, false);

  if (docker.isError()) {
    return process::Failure(docker.error());
  }

  const Try<std::string> directory = os::mkdtemp();
  if (directory.isError()) {
    return process::Failure(docker.error());
  }

  return docker.get()->pull(directory.get(), imageName)
    .then([]() {
      // `Docker::pull` returns a `Future<Docker::Image`>, but we only really
      // if the pull was successful, so we just return `Nothing` to match the
      // return type of `pullDockerImage`.
      return Nothing();
    })
    .onAny([directory]() -> process::Future<Nothing> {
      Try<Nothing> rmdir = os::rmdir(directory.get());
      if (rmdir.isError()) {
        return process::Failure(rmdir.error());
      }
      return Nothing();
    });
}


inline void createDockerIPv6UserNetwork()
{
  // Docker IPv6 is not supported on Windows, so no-op on that platform.
  // TODO(akagup): Remove the #ifdef when Windows supports IPv6 networks
  // in docker containers. See MESOS-8566.
#ifndef __WINDOWS__
  // Create a Docker user network with IPv6 enabled.
  Try<std::string> dockerCommand = strings::format(
      "docker network create --driver=bridge --ipv6 "
      "--subnet=fd01::/64 %s",
      DOCKER_IPv6_NETWORK);

  Try<process::Subprocess> s = process::subprocess(
      dockerCommand.get(),
      process::Subprocess::PATH("/dev/null"),
      process::Subprocess::PATH("/dev/null"),
      process::Subprocess::PIPE());

  ASSERT_SOME(s) << "Unable to create the Docker IPv6 network: "
                 << DOCKER_IPv6_NETWORK;

  process::Future<std::string> err = process::io::read(s->err().get());

  // Wait for the network to be created.
  AWAIT_READY(s->status());
  AWAIT_READY(err);

  ASSERT_SOME(s->status().get());
  ASSERT_EQ(s->status()->get(), 0)
    << "Unable to create the Docker IPv6 network "
    << DOCKER_IPv6_NETWORK
    << " : " << err.get();
#endif // __WINDOWS__
}


inline void removeDockerIPv6UserNetwork()
{
  // Docker IPv6 is not supported on Windows, so no-op on that platform.
  // TODO(akagup): Remove the #ifdef when Windows supports IPv6 networks
  // in docker containers. See MESOS-8566.
#ifndef __WINDOWS__
  // Delete the Docker user network.
  Try<std::string> dockerCommand =
    strings::format("docker network rm %s", DOCKER_IPv6_NETWORK);

  Try<process::Subprocess> s = subprocess(
      dockerCommand.get(),
      process::Subprocess::PATH("/dev/null"),
      process::Subprocess::PATH("/dev/null"),
      process::Subprocess::PIPE());

  // This is best effort cleanup. In case of an error just a log an
  // error.
  ASSERT_SOME(s) << "Unable to delete the Docker IPv6 network: "
                 << DOCKER_IPv6_NETWORK;

  process::Future<std::string> err = process::io::read(s->err().get());

  // Wait for the network to be deleted.
  AWAIT_READY(s->status());
  AWAIT_READY(err);

  ASSERT_SOME(s->status().get());
  ASSERT_EQ(s->status()->get(), 0)
    << "Unable to delete the Docker IPv6 network "
    << DOCKER_IPv6_NETWORK
    << " : " << err.get();
#endif // __WINDOWS__
}


inline void assertDockerKillStatus(process::Future<Option<int>>& status)
{
#ifdef __WINDOWS__
  // On Windows, there is no standard exit code for determining if a process
  // as been killed. However, in Docker, it will not return 0.
  AWAIT_EXPECT_WEXITSTATUS_NE(0, status.get());
#else
  AWAIT_EXPECT_WEXITSTATUS_EQ(128 + SIGKILL, status);
#endif // __WINDOWS__
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_DOCKER_COMMON_HPP__
