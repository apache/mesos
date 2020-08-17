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

#ifndef __CSI_V1_VOLUME_MANAGER_PROCESS_HPP__
#define __CSI_V1_VOLUME_MANAGER_PROCESS_HPP__

#include <string>
#include <vector>

#include <google/protobuf/map.h>

#include <mesos/mesos.hpp>

#include <mesos/secret/resolver.hpp>

#include <process/future.hpp>
#include <process/grpc.hpp>
#include <process/http.hpp>
#include <process/loop.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/sequence.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "csi/metrics.hpp"
#include "csi/service_manager.hpp"
#include "csi/state.hpp"
#include "csi/v1_client.hpp"
#include "csi/v1_utils.hpp"
#include "csi/v1_volume_manager.hpp"
#include "csi/volume_manager.hpp"

namespace mesos {
namespace csi {
namespace v1 {

class VolumeManagerProcess : public process::Process<VolumeManagerProcess>
{
public:
  explicit VolumeManagerProcess(
      const std::string& _rootDir,
      const CSIPluginInfo& _info,
      const hashset<Service> _services,
      const process::grpc::client::Runtime& _runtime,
      ServiceManager* _serviceManager,
      Metrics* _metrics,
      SecretResolver* _secretResolver);

  process::Future<Nothing> recover();

  process::Future<std::vector<VolumeInfo>> listVolumes();

  process::Future<Bytes> getCapacity(
      const CSIVolume::VolumeCapability& capability,
      const google::protobuf::Map<std::string, std::string>& parameters);

  process::Future<VolumeInfo> createVolume(
      const std::string& name,
      const Bytes& capacity,
      const CSIVolume::VolumeCapability& capability,
      const google::protobuf::Map<std::string, std::string>& parameters);

  process::Future<Option<Error>> validateVolume(
      const VolumeInfo& volumeInfo,
      const CSIVolume::VolumeCapability& capability,
      const google::protobuf::Map<std::string, std::string>& parameters);

  process::Future<bool> deleteVolume(const std::string& volumeId);

  process::Future<Nothing> attachVolume(const std::string& volumeId);

  process::Future<Nothing> detachVolume(const std::string& volumeId);

  process::Future<Nothing> publishVolume(
      const std::string& volumeId,
      const Option<state::VolumeState>& volumeState = None());

  process::Future<Nothing> unpublishVolume(const std::string& volumeId);

  // Wrapper functions to make CSI calls and update RPC metrics. Made public for
  // testing purpose.
  //
  // The call is made asynchronously and thus no guarantee is provided on the
  // order in which calls are sent. Callers need to either ensure to not have
  // multiple conflicting calls in flight, or treat results idempotently.
  //
  // NOTE: We currently ensure this by 1) resource locking to forbid concurrent
  // calls on the same volume, and 2) no profile update while there are ongoing
  // `CREATE_DISK` or `DESTROY_DISK` operations.
  template <typename Request, typename Response>
  process::Future<Response> call(
      const Service& service,
      process::Future<RPCResult<Response>> (Client::*rpc)(Request),
      const Request& request,
      bool retry = false);

  template <typename Request, typename Response>
  process::Future<RPCResult<Response>> _call(
      const std::string& endpoint,
      process::Future<RPCResult<Response>> (Client::*rpc)(Request),
      const Request& request);

  template <typename Response>
  process::Future<process::ControlFlow<Response>> __call(
      const RPCResult<Response>& result, const Option<Duration>& backoff);

private:
  process::Future<Nothing> prepareServices();

  process::Future<bool> _deleteVolume(const std::string& volumeId);
  process::Future<bool> __deleteVolume(const std::string& volumeId);

  // The following methods are used to manage volume lifecycles. Transient
  // states are omitted.
  //
  //                          +------------+
  //                 +  +  +  |  CREATED   |  ^
  //                 |  |  |  +---+----^---+  |
  //   _attachVolume |  |  |      |    |      | _detachVolume
  //                 |  |  |  +---v----+---+  |
  //                 v  +  +  | NODE_READY |  +  ^
  //                    |  |  +---+----^---+  |  |
  //    __publishVolume |  |      |    |      |  | _unpublishVolume
  //                    |  |  +---v----+---+  |  |
  //                    v  +  | VOL_READY  |  +  +  ^
  //                       |  +---+----^---+  |  |  |
  //        _publishVolume |      |    |      |  |  | __unpublishVolume
  //                       |  +---v----+---+  |  |  |
  //                       V  | PUBLISHED  |  +  +  +
  //                          +------------+

  // Transition a volume to `NODE_READY` state from any state above.
  process::Future<Nothing> _attachVolume(const std::string& volumeId);

  // Transition a volume to `CREATED` state from any state below.
  process::Future<Nothing> _detachVolume(const std::string& volumeId);

  // Transition a volume to `PUBLISHED` state from any state above.
  process::Future<Nothing> _publishVolume(const std::string& volumeId);

  // Transition a volume to `VOL_READY` state from any state above.
  process::Future<Nothing> __publishVolume(const std::string& volumeId);

  // Transition a volume to `NODE_READY` state from any state below.
  process::Future<Nothing> _unpublishVolume(const std::string& volumeId);

  // Transition a volume to `VOL_READY` state from any state below.
  process::Future<Nothing> __unpublishVolume(const std::string& volumeId);

  void checkpointVolumeState(const std::string& volumeId);

  void garbageCollectMountPath(const std::string& volumeId);

  // Removes the metadata associated with a particular volume both
  // from memory and from disk.
  void removeVolume(const std::string& volumeId);

  // If the volume manager was initialized with a non-null secret resolver, this
  // helper function will resolve any secrets in the provided map.
  // Returns a map containing the resolved secrets.
  process::Future<google::protobuf::Map<std::string, std::string>>
    resolveSecrets(
        const google::protobuf::Map<std::string, Secret>& secrets);

  const std::string rootDir;
  const CSIPluginInfo info;
  const hashset<Service> services;

  process::grpc::client::Runtime runtime;
  ServiceManager* serviceManager;
  Metrics* metrics;
  SecretResolver* secretResolver;
  const std::string mountRootDir;

  Option<std::string> bootId;
  Option<PluginCapabilities> pluginCapabilities;
  Option<ControllerCapabilities> controllerCapabilities;
  Option<NodeCapabilities> nodeCapabilities;
  Option<std::string> nodeId;

  struct VolumeData
  {
    VolumeData(state::VolumeState&& _state)
      : state(_state), sequence(new process::Sequence("csi-volume-sequence")) {}

    state::VolumeState state;

    // We call all CSI operations on the same volume in a sequence to ensure
    // that they are processed in a sequential order.
    process::Owned<process::Sequence> sequence;
  };

  hashmap<std::string, VolumeData> volumes;
};

} // namespace v1 {
} // namespace csi {
} // namespace mesos {

#endif // __CSI_V1_VOLUME_MANAGER_PROCESS_HPP__
