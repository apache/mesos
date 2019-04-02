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

#include "csi/volume_manager.hpp"

#include <process/grpc.hpp>

#include "csi/v0_volume_manager.hpp"

namespace http = process::http;

using std::string;

using process::Owned;

using process::grpc::client::Runtime;

namespace mesos {
namespace csi {

Try<Owned<VolumeManager>> VolumeManager::create(
    const http::URL& agentUrl,
    const string& rootDir,
    const CSIPluginInfo& info,
    const hashset<Service>& services,
    const string& containerPrefix,
    const Option<string>& authToken,
    Metrics* metrics)
{
  if (services.empty()) {
    return Error(
        "Must specify at least one service for CSI plugin type '" +
        info.type() + "' and name '" + info.name() + "'");
  }

  return new v0::VolumeManager(
      agentUrl,
      rootDir,
      info,
      services,
      containerPrefix,
      authToken,
      Runtime(),
      metrics);
}

} // namespace csi {
} // namespace mesos {
