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

#include "slave/container_daemon.hpp"

#include <process/defer.hpp>
#include <process/id.hpp>

#include <stout/lambda.hpp>
#include <stout/stringify.hpp>
#include <stout/unreachable.hpp>

#include "internal/evolve.hpp"

#include "slave/container_daemon_process.hpp"

namespace http = process::http;

using std::string;

using mesos::agent::Call;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::Promise;

using process::defer;

namespace mesos {
namespace internal {
namespace slave {

// Returns the 'Bearer' credential as a header for calling the V1 agent
// API if the `authToken` is presented, or empty otherwise.
// TODO(chhsiao): Currently we assume the JWT authenticator is used for
// the agent operator API.
static inline http::Headers getAuthHeader(const Option<string>& authToken)
{
  http::Headers headers;

  if (authToken.isSome()) {
    headers["Authorization"] = "Bearer " + authToken.get();
  }

  return headers;
}


ContainerDaemonProcess::ContainerDaemonProcess(
    const http::URL& _agentUrl,
    const Option<string>& _authToken,
    const ContainerID& containerId,
    const Option<CommandInfo>& commandInfo,
    const Option<Resources>& resources,
    const Option<ContainerInfo>& containerInfo,
    const Option<std::function<Future<Nothing>()>>& _postStartHook,
    const Option<std::function<Future<Nothing>()>>& _postStopHook)
  : ProcessBase(process::ID::generate("container-daemon")),
    agentUrl(_agentUrl),
    authToken(_authToken),
    contentType(ContentType::PROTOBUF),
    postStartHook(_postStartHook),
    postStopHook(_postStopHook)
{
  launchCall.set_type(Call::LAUNCH_CONTAINER);
  launchCall.mutable_launch_container()
    ->mutable_container_id()->CopyFrom(containerId);

  if (commandInfo.isSome()) {
    launchCall.mutable_launch_container()
      ->mutable_command()->CopyFrom(commandInfo.get());
  }

  if (resources.isSome()) {
    launchCall.mutable_launch_container()
      ->mutable_resources()->CopyFrom(resources.get());
  }

  if (containerInfo.isSome()) {
    launchCall.mutable_launch_container()
      ->mutable_container()->CopyFrom(containerInfo.get());
  }

  waitCall.set_type(Call::WAIT_CONTAINER);
  waitCall.mutable_wait_container()->mutable_container_id()->CopyFrom(
      containerId);
}


Future<Nothing> ContainerDaemonProcess::wait()
{
  return terminated.future();
}


void ContainerDaemonProcess::initialize()
{
  launchContainer();
}


void ContainerDaemonProcess::launchContainer()
{
  const ContainerID& containerId = launchCall.launch_container().container_id();

  LOG(INFO) << "Launching container '" << containerId << "'";

  http::post(
      agentUrl,
      getAuthHeader(authToken),
      serialize(contentType, evolve(launchCall)),
      stringify(contentType))
    .then(defer(self(), [=](
        const http::Response& response) -> Future<Nothing> {
      if (response.status != http::OK().status &&
          response.status != http::Accepted().status) {
        return Failure(
            "Failed to launch container '" +
            stringify(launchCall.launch_container().container_id()) +
            "': Unexpected response '" + response.status + "' (" +
            response.body + ")");
      }

      if (postStartHook.isSome()) {
        LOG(INFO)
          << "Invoking post-start hook for container '" << containerId << "'";

        return postStartHook.get()();
      }

      return Nothing();
    }))
    .onReady(defer(self(), &Self::waitContainer))
    .onFailed(defer(self(), [=](const string& failure) {
      LOG(ERROR)
        << "Failed to launch container '"
        << launchCall.launch_container().container_id() << "': " << failure;

      terminated.fail(failure);
    }))
    .onDiscarded(defer(self(), [=] {
      LOG(ERROR)
        << "Failed to launch container '"
        << launchCall.launch_container().container_id()
        << "': future discarded";

      terminated.discard();
    }));
}


void ContainerDaemonProcess::waitContainer()
{
  const ContainerID& containerId = waitCall.wait_container().container_id();

  LOG(INFO) << "Waiting for container '" << containerId << "'";

  http::post(
      agentUrl,
      getAuthHeader(authToken),
      serialize(contentType, evolve(waitCall)),
      stringify(contentType))
    .then(defer(self(), [=](const http::Response& response) -> Future<Nothing> {
      if (response.status != http::OK().status &&
          response.status != http::NotFound().status) {
        return Failure(
            "Failed to wait for container '" +
            stringify(waitCall.wait_container().container_id()) +
            "': Unexpected response '" + response.status + "' (" +
            response.body + ")");
      }

      if (postStopHook.isSome()) {
        LOG(INFO)
          << "Invoking post-stop hook for container '" << containerId << "'";

        return postStopHook.get()();
      }

      return Nothing();
    }))
    .onReady(defer(self(), &Self::launchContainer))
    .onFailed(defer(self(), [=](const string& failure) {
      LOG(ERROR)
        << "Failed to wait for container '"
        << waitCall.wait_container().container_id() << "': " << failure;

      terminated.fail(failure);
    }))
    .onDiscarded(defer(self(), [=] {
      LOG(ERROR)
        << "Failed to wait for container '"
        << waitCall.wait_container().container_id() << "': future discarded";

      terminated.discard();
    }));
}


Try<Owned<ContainerDaemon>> ContainerDaemon::create(
    const http::URL& agentUrl,
    const Option<string>& authToken,
    const ContainerID& containerId,
    const Option<CommandInfo>& commandInfo,
    const Option<Resources>& resources,
    const Option<ContainerInfo>& containerInfo,
    const Option<std::function<Future<Nothing>()>>& postStartHook,
    const Option<std::function<Future<Nothing>()>>& postStopHook)
{
  return Owned<ContainerDaemon>(new ContainerDaemon(
      agentUrl,
      authToken,
      containerId,
      commandInfo,
      resources,
      containerInfo,
      postStartHook,
      postStopHook));
}


ContainerDaemon::ContainerDaemon(
    const http::URL& agentUrl,
    const Option<string>& authToken,
    const ContainerID& containerId,
    const Option<CommandInfo>& commandInfo,
    const Option<Resources>& resources,
    const Option<ContainerInfo>& containerInfo,
    const Option<std::function<Future<Nothing>()>>& postStartHook,
    const Option<std::function<Future<Nothing>()>>& postStopHook)
  : process(new ContainerDaemonProcess(
        agentUrl,
        authToken,
        containerId,
        commandInfo,
        resources,
        containerInfo,
        postStartHook,
        postStopHook))
{
  spawn(CHECK_NOTNULL(process.get()));
}


ContainerDaemon::~ContainerDaemon()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<Nothing> ContainerDaemon::wait()
{
  return process->wait();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
