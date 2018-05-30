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

#include "csi/rpc.hpp"

#include <stout/unreachable.hpp>

using std::ostream;

namespace mesos {
namespace csi {
namespace v0 {

ostream& operator<<(ostream& stream, const RPC& rpc)
{
  switch (rpc) {
    case GET_PLUGIN_INFO:
      return stream
        << Identity::service_full_name()
        << ".GetPluginInfo";
    case GET_PLUGIN_CAPABILITIES:
      return stream
        << Identity::service_full_name()
        << ".GetPluginCapabilities";
    case PROBE:
      return stream
        << Identity::service_full_name()
        << ".Probe";
    case CREATE_VOLUME:
      return stream
        << Controller::service_full_name()
        << ".CreateVolume";
    case DELETE_VOLUME:
      return stream
        << Controller::service_full_name()
        << ".DeleteVolume";
    case CONTROLLER_PUBLISH_VOLUME:
      return stream
        << Controller::service_full_name()
        << ".ControllerPublishVolume";
    case CONTROLLER_UNPUBLISH_VOLUME:
      return stream
        << Controller::service_full_name()
        << ".ControllerUnpublishVolume";
    case VALIDATE_VOLUME_CAPABILITIES:
      return stream
        << Controller::service_full_name()
        << ".ValidateVolumeCapabilities";
    case LIST_VOLUMES:
      return stream
        << Controller::service_full_name()
        << ".ListVolumes";
    case GET_CAPACITY:
      return stream
        << Controller::service_full_name()
        << ".GetCapacity";
    case CONTROLLER_GET_CAPABILITIES:
      return stream
        << Controller::service_full_name()
        << ".ControllerGetCapabilities";
    case NODE_STAGE_VOLUME:
      return stream
        << Node::service_full_name()
        << ".NodeStageVolume";
    case NODE_UNSTAGE_VOLUME:
      return stream
        << Node::service_full_name()
        << ".NodeUnstageVolume";
    case NODE_PUBLISH_VOLUME:
      return stream
        << Node::service_full_name()
        << ".NodePublishVolume";
    case NODE_UNPUBLISH_VOLUME:
      return stream
        << Node::service_full_name()
        << ".NodeUnpublishVolume";
    case NODE_GET_ID:
      return stream
        << Node::service_full_name()
        << ".NodeGetId";
    case NODE_GET_CAPABILITIES:
      return stream
        << Node::service_full_name()
        << ".NodeGetCapabilities";
  }

  UNREACHABLE();
}

} // namespace v0 {
} // namespace csi {
} // namespace mesos {
