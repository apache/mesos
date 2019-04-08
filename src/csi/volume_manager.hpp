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

#ifndef __CSI_VOLUME_MANAGER_HPP__
#define __CSI_VOLUME_MANAGER_HPP__

#include <string>
#include <vector>

#include <google/protobuf/map.h>

#include <mesos/mesos.hpp>

#include <mesos/csi/types.hpp>

#include <process/future.hpp>
#include <process/grpc.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/bytes.hpp>
#include <stout/error.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "csi/metrics.hpp"
#include "csi/service_manager.hpp"

namespace mesos {
namespace csi {

struct VolumeInfo
{
  Bytes capacity;
  std::string id;
  google::protobuf::Map<std::string, std::string> context;
};


// Manages the volumes of a CSI plugin instance.
class VolumeManager
{
public:
  static Try<process::Owned<VolumeManager>> create(
      const std::string& rootDir,
      const CSIPluginInfo& info,
      const hashset<Service>& services,
      const std::string& apiVersion,
      const process::grpc::client::Runtime& runtime,
      ServiceManager* serviceManager,
      Metrics* metrics);

  virtual ~VolumeManager() = default;

  virtual process::Future<Nothing> recover() = 0;

  // Lists all volumes, which may include untracked ones. Returns an empty list
  // if `LIST_VOLUMES` controller capability is not supported.
  virtual process::Future<std::vector<VolumeInfo>> listVolumes() = 0;

  // Returns the capacity that can be used to provision volumes for the
  // given capability and parameters. Returns zero bytes if `GET_CAPACITY`
  // controller capability is not supported.
  virtual process::Future<Bytes> getCapacity(
      const types::VolumeCapability& capability,
      const google::protobuf::Map<std::string, std::string>& parameters) = 0;

  // The following methods are used to manage volume lifecycles. The lifecycle
  // of a volume is shown as follows, where unboxed states are transient states
  // that might be skipped depending on the plugin's capabilities.
  //
  //                  +------------+
  //                  |  CREATED   |
  //                  +---+----^---+
  //   CONTROLLER_PUBLISH |    | CONTROLLER_UNPUBLISH
  //                  +---v----+---+
  //                  | NODE_READY |
  //                  +---+----^---+
  //           NODE_STAGE |    | NODE_UNSTAGE
  //                  +---v----+---+
  //                  | VOL_READY  |
  //                  +---+----^---+
  //         NODE_PUBLISH |    | NODE_UNPUBLISH
  //                  +---v----+---+
  //                  | PUBLISHED  |
  //                  +------------+

  // Provisions and tracks a new volume in `CREATED` state.
  virtual process::Future<VolumeInfo> createVolume(
      const std::string& name,
      const Bytes& capacity,
      const types::VolumeCapability& capability,
      const google::protobuf::Map<std::string, std::string>& parameters) = 0;

  // Validates a volume against the given capability and parameters. Once
  // validated, tracks the volume in `CREATED` state if it is previously
  // untracked then returns None. Otherwise returns the validation error.
  virtual process::Future<Option<Error>> validateVolume(
      const VolumeInfo& volumeInfo,
      const types::VolumeCapability& capability,
      const google::protobuf::Map<std::string, std::string>& parameters) = 0;

  // Deprovisions a volume and returns true if `CREATE_DELETE_VOLUME` controller
  // capability is supported. Otherwise, transitions the volume to `CREATED`
  // state and untracks it if it is previously tracked then returns false.
  virtual process::Future<bool> deleteVolume(const std::string& volumeId) = 0;

  // Transitions a tracked volume to `NODE_READY` state from any state above.
  virtual process::Future<Nothing> attachVolume(
      const std::string& volumeId) = 0;

  // Transitions a tracked volume to `CREATED` state from any state below.
  virtual process::Future<Nothing> detachVolume(
      const std::string& volumeId) = 0;

  // Transitions a tracked volume to `PUBLISHED` state from any state above.
  virtual process::Future<Nothing> publishVolume(
      const std::string& volumeId) = 0;

  // Transitions a tracked volume to `NODE_READY` state from any state below.
  virtual process::Future<Nothing> unpublishVolume(
      const std::string& volumeId) = 0;
};

} // namespace csi {
} // namespace mesos {

#endif // __CSI_VOLUME_MANAGER_HPP__
