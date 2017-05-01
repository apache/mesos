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

#include <string>

#include <mesos/slave/container_logger.hpp>

#include <process/owned.hpp>
#include <process/shared.hpp>

#include <stout/json.hpp>
#include <stout/option.hpp>

#include "slave/containerizer/docker.hpp"

#include "tests/mock_docker.hpp"

using mesos::slave::ContainerLogger;

using std::string;

using process::Owned;
using process::Shared;

namespace mesos {
namespace internal {
namespace tests {

MockDocker::MockDocker(
    const string& path,
    const string& socket,
    const Option<JSON::Object>& config)
  : Docker(path, socket, config)
{
  EXPECT_CALL(*this, ps(_, _))
    .WillRepeatedly(Invoke(this, &MockDocker::_ps));

  EXPECT_CALL(*this, pull(_, _, _))
    .WillRepeatedly(Invoke(this, &MockDocker::_pull));

  EXPECT_CALL(*this, stop(_, _, _))
    .WillRepeatedly(Invoke(this, &MockDocker::_stop));

  EXPECT_CALL(*this, run(_, _, _))
    .WillRepeatedly(Invoke(this, &MockDocker::_run));

  EXPECT_CALL(*this, inspect(_, _))
    .WillRepeatedly(Invoke(this, &MockDocker::_inspect));
}


MockDocker::~MockDocker() {}


MockDockerContainerizer::MockDockerContainerizer(
    const slave::Flags& flags,
    slave::Fetcher* fetcher,
    const Owned<ContainerLogger>& logger,
    Shared<Docker> docker,
    const Option<NvidiaComponents>& nvidia)
  : slave::DockerContainerizer(
      flags, fetcher, logger, docker, nvidia)
{
  initialize();
}


MockDockerContainerizer::MockDockerContainerizer(
    const Owned<slave::DockerContainerizerProcess>& process)
  : slave::DockerContainerizer(process)
{
  initialize();
}


MockDockerContainerizer::~MockDockerContainerizer() {}


MockDockerContainerizerProcess::MockDockerContainerizerProcess(
    const slave::Flags& flags,
    slave::Fetcher* fetcher,
    const Owned<ContainerLogger>& logger,
    const Shared<Docker>& docker,
    const Option<NvidiaComponents>& nvidia)
  : slave::DockerContainerizerProcess(
      flags, fetcher, logger, docker, nvidia)
{
  EXPECT_CALL(*this, fetch(_))
    .WillRepeatedly(Invoke(this, &MockDockerContainerizerProcess::_fetch));

  EXPECT_CALL(*this, pull(_))
    .WillRepeatedly(Invoke(this, &MockDockerContainerizerProcess::_pull));
}


MockDockerContainerizerProcess::~MockDockerContainerizerProcess() {}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
