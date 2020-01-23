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

#ifndef __SLAVE_HTTP_HPP__
#define __SLAVE_HTTP_HPP__

#include <process/authenticator.hpp>
#include <process/http.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/limiter.hpp>

#include <stout/json.hpp>
#include <stout/option.hpp>

#include <mesos/authorizer/authorizer.hpp>

#include "common/http.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declarations.
class Slave;


// HTTP route handlers.
class Http
{
public:
  explicit Http(Slave* _slave)
    : slave(_slave),
      statisticsLimiter(new process::RateLimiter(2, Seconds(1))) {}

  // /api/v1
  process::Future<process::http::Response> api(
      const process::http::Request& request,
      const Option<process::http::authentication::Principal>& principal) const;

  // /api/v1/executor
  process::Future<process::http::Response> executor(
      const process::http::Request& request,
      const Option<process::http::authentication::Principal>& principal) const;

  // /slave/flags
  process::Future<process::http::Response> flags(
      const process::http::Request& request,
      const Option<process::http::authentication::Principal>& principal) const;

  // /slave/health
  process::Future<process::http::Response> health(
      const process::http::Request& request) const;

  // /slave/state
  process::Future<process::http::Response> state(
      const process::http::Request& request,
      const Option<process::http::authentication::Principal>&) const;

  // /slave/monitor/statistics
  process::Future<process::http::Response> statistics(
      const process::http::Request& request,
      const Option<process::http::authentication::Principal>& principal) const;

  // /slave/containers
  process::Future<process::http::Response> containers(
      const process::http::Request& request,
      const Option<process::http::authentication::Principal>& principal) const;

  // /slave/containerizer/debug
  process::Future<process::http::Response> containerizerDebug(
      const process::http::Request& request,
      const Option<process::http::authentication::Principal>& principal) const;

  static std::string API_HELP();
  static std::string EXECUTOR_HELP();
  static std::string RESOURCE_PROVIDER_HELP();
  static std::string FLAGS_HELP();
  static std::string HEALTH_HELP();
  static std::string STATE_HELP();
  static std::string STATISTICS_HELP();
  static std::string CONTAINERS_HELP();
  static std::string CONTAINERIZER_DEBUG_HELP();

private:
  JSON::Object _flags() const;

  // Continuation for `/api` endpoint that handles streaming and non-streaming
  // requests. In case of a streaming request, `call` would be the first
  // record and additional records can be read using the `reader`. For
  // non-streaming requests, `reader` would be set to `None()`.
  process::Future<process::http::Response> _api(
      const agent::Call& call,
      Option<process::Owned<recordio::Reader<agent::Call>>>&& reader,
      const RequestMediaTypes& mediaTypes,
      const Option<process::http::authentication::Principal>& principal) const;

  // Make continuation for `statistics` `static` as it might
  // execute when the invoking `Http` is already destructed.
  process::http::Response _statistics(
      const ResourceUsage& usage,
      const process::http::Request& request) const;

  // Continuation for `/containers` endpoint
  process::Future<process::http::Response> _containers(
      const process::http::Request& request,
      const Option<process::http::authentication::Principal>& principal) const;

  // Helper function to collect containers status and resource statistics.
  process::Future<JSON::Array> __containers(
      const process::Owned<ObjectApprovers>& approvers,
      Option<IDAcceptor<ContainerID>> selectContainerId,
      bool showNestedContainers,
      bool showStandaloneContainers) const;

  // Continuation for `/containerizer/debug` endpoint
  process::Future<JSON::Object> _containerizerDebug() const;

  // Helper routines for endpoint authorization.
  Try<std::string> extractEndpoint(const process::http::URL& url) const;

  // Agent API handlers.
  process::Future<process::http::Response> getFlags(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> getHealth(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> getVersion(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> getMetrics(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> getLoggingLevel(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> setLoggingLevel(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> listFiles(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> getContainers(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> readFile(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> getFrameworks(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  std::function<void(JSON::ObjectWriter*)> jsonifyGetFrameworks(
      const process::Owned<ObjectApprovers>& approvers) const;
  std::string serializeGetFrameworks(
      const process::Owned<ObjectApprovers>& approvers) const;

  process::Future<process::http::Response> getExecutors(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  std::function<void(JSON::ObjectWriter*)> jsonifyGetExecutors(
      const process::Owned<ObjectApprovers>& approvers) const;
  std::string serializeGetExecutors(
      const process::Owned<ObjectApprovers>& approvers) const;

  process::Future<process::http::Response> getOperations(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> getTasks(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  std::function<void(JSON::ObjectWriter*)> jsonifyGetTasks(
      const process::Owned<ObjectApprovers>& approvers) const;
  std::string serializeGetTasks(
      const process::Owned<ObjectApprovers>& approvers) const;

  process::Future<process::http::Response> getAgent(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> getResourceProviders(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> getState(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  std::function<void(JSON::ObjectWriter*)> jsonifyGetState(
      const process::Owned<ObjectApprovers>& approvers) const;
  std::string serializeGetState(
      const process::Owned<ObjectApprovers>& approvers) const;

  process::Future<process::http::Response> launchNestedContainer(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> launchContainer(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  template <mesos::authorization::Action action>
  process::Future<process::http::Response> launchContainer(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  template <mesos::authorization::Action action>
  process::Future<process::http::Response> _launchContainer(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const Option<Resources>& resourceRequests,
      const Option<
          google::protobuf::Map<std::string, Value::Scalar>>& resourceLimits,
      const Option<ContainerInfo>& containerInfo,
      const Option<mesos::slave::ContainerClass>& containerClass,
      ContentType acceptType,
      const process::Owned<ObjectApprovers>& approvers) const;

  process::Future<process::http::Response> waitNestedContainer(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> waitContainer(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  template <authorization::Action action>
  process::Future<process::http::Response> waitContainer(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  template <authorization::Action action>
  process::Future<process::http::Response> _waitContainer(
      const ContainerID& containerId,
      ContentType acceptType,
      const process::Owned<ObjectApprovers>& approvers,
      const bool deprecated) const;

  process::Future<process::http::Response> killNestedContainer(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> killContainer(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  template <mesos::authorization::Action ACTION>
  process::Future<process::http::Response> killContainer(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  template <mesos::authorization::Action ACTION>
  process::Future<process::http::Response> _killContainer(
      const ContainerID& containerId,
      const int signal,
      ContentType acceptType,
      const process::Owned<ObjectApprovers>& approvers) const;

  process::Future<process::http::Response> removeNestedContainer(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> removeContainer(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  template <mesos::authorization::Action ACTION>
  process::Future<process::http::Response> removeContainer(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>& principal) const;

  template <mesos::authorization::Action ACTION>
  process::Future<process::http::Response> _removeContainer(
      const ContainerID& containerId,
      ContentType acceptType,
      const process::Owned<ObjectApprovers>& approvers) const;

  process::Future<process::http::Response> launchNestedContainerSession(
      const mesos::agent::Call& call,
      const RequestMediaTypes& mediaTypes,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> attachContainerInput(
      const mesos::agent::Call& call,
      process::Owned<recordio::Reader<agent::Call>>&& decoder,
      const RequestMediaTypes& mediaTypes,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> _attachContainerInput(
      const mesos::agent::Call& call,
      process::Owned<recordio::Reader<agent::Call>>&& decoder,
      const RequestMediaTypes& mediaTypes) const;

  process::Future<process::http::Response> acknowledgeContainerInputResponse(
      const ContainerID& containerId) const;

  process::Future<process::http::Response> attachContainerOutput(
      const mesos::agent::Call& call,
      const RequestMediaTypes& mediaTypes,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> _attachContainerOutput(
      const mesos::agent::Call& call,
      const RequestMediaTypes& mediaTypes) const;

  process::Future<process::http::Response> addResourceProviderConfig(
      const mesos::agent::Call& call,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> updateResourceProviderConfig(
      const mesos::agent::Call& call,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> removeResourceProviderConfig(
      const mesos::agent::Call& call,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> markResourceProviderGone(
      const mesos::agent::Call& call,
      const Option<process::http::authentication::Principal>& principal) const;

  process::Future<process::http::Response> pruneImages(
      const mesos::agent::Call& call,
      ContentType acceptType,
      const Option<process::http::authentication::Principal>&
          principal) const;

  Slave* slave;

  // Used to rate limit the statistics endpoint.
  process::Shared<process::RateLimiter> statisticsLimiter;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_HTTP_HPP__
