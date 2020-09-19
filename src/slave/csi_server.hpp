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

#ifndef __SLAVE_CSI_SERVER_HPP__
#define __SLAVE_CSI_SERVER_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <mesos/authentication/secret_generator.hpp>

#include <mesos/secret/resolver.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/hashmap.hpp>
#include <stout/try.hpp>

#include "csi/service_manager.hpp"
#include "csi/volume_manager.hpp"

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {

class CSIServerProcess;

// A CSI server is a collection of volume managers and associated service
// managers. This object can be instantiated and held by the Mesos agent to
// manage a collection of CSI plugins and proxy calls to them.
class CSIServer
{
public:
  ~CSIServer();

  static Try<process::Owned<CSIServer>> create(
      const Flags& flags,
      const process::http::URL& agentUrl,
      SecretGenerator* secretGenerator,
      SecretResolver* secretResolver);

  // Starts the CSI server. Any `publishVolume()` or `unpublishVolume()` calls
  // which were made previously will be executed after this method is called.
  // Returns a future which is satisfied once initialization is complete.
  process::Future<Nothing> start(const SlaveID& agentId);

  // Publish a CSI volume to this agent. If the `start()` method has not yet
  // been called, then the publishing of this volume will not be completed until
  // the CSI server is started.
  // Returns the target path at which the volume has been published.
  process::Future<std::string> publishVolume(const Volume& volume);

  // Unpublishes a CSI volume from this agent. If the `start()` method has not
  // yet been called, then the unpublishing of this volume will not be completed
  // until the CSI server is started.
  process::Future<Nothing> unpublishVolume(
      const std::string& pluginName,
      const std::string& volumeId);

private:
  CSIServer(
      const process::http::URL& agentUrl,
      const std::string& rootDir,
      const std::string& pluginConfigDir,
      SecretGenerator* secretGenerator,
      SecretResolver* secretResolver);

  process::Owned<CSIServerProcess> process;

  process::Promise<Nothing> started;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CSI_SERVER_HPP__
