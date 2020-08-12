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

#ifndef __CSI_SERVICE_MANAGER_HPP__
#define __CSI_SERVICE_MANAGER_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/grpc.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

#include "csi/metrics.hpp"

namespace mesos {
namespace csi {

using Service = CSIPluginContainerInfo::Service;

constexpr Service CONTROLLER_SERVICE =
  CSIPluginContainerInfo::CONTROLLER_SERVICE;

constexpr Service NODE_SERVICE = CSIPluginContainerInfo::NODE_SERVICE;


// Forward declarations.
class ServiceManagerProcess;


// Manages the services of a CSI plugin instance.
class ServiceManager
{
public:
  // This is for the managed CSI plugins which will be
  // launched as standalone containers.
  ServiceManager(
      const SlaveID& agentId,
      const process::http::URL& agentUrl,
      const std::string& rootDir,
      const CSIPluginInfo& info,
      const hashset<Service>& services,
      const std::string& containerPrefix,
      const Option<std::string>& authToken,
      const process::grpc::client::Runtime& runtime,
      Metrics* metrics);

  // This is for the unmanaged CSI plugins which we assume
  // are already launched out of Mesos.
  ServiceManager(
      const CSIPluginInfo& info,
      const hashset<Service>& services,
      const process::grpc::client::Runtime& runtime,
      Metrics* metrics);

  // Since this class contains `Owned` members which should not but can be
  // copied, explicitly make this class non-copyable.
  //
  // TODO(chhsiao): Remove this once MESOS-5122 is fixed.
  ServiceManager(const ServiceManager&) = delete;
  ServiceManager& operator=(const ServiceManager&) = delete;

  ~ServiceManager();

  process::Future<Nothing> recover();

  process::Future<std::string> getServiceEndpoint(const Service& service);
  process::Future<std::string> getApiVersion();

private:
  process::Owned<ServiceManagerProcess> process;
  process::Future<Nothing> recovered;
};

} // namespace csi {
} // namespace mesos {

#endif // __CSI_SERVICE_MANAGER_HPP__
