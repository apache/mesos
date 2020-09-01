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

#ifndef __VOLUME_CSI_ISOLATOR_PATHS_HPP__
#define __VOLUME_CSI_ISOLATOR_PATHS_HPP__

#include <string>

#include <mesos/mesos.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace csi {
namespace paths {

// The root directory where we keep the information of CSI volumes that each
// container uses. The layout is as follows:
//   /<work_dir>/isolators/volume/csi/
//      |-- <ID of Container1>/
//      |      |-- volumes
//      |-- <ID of Container2>/
//      |      |-- volumes
//      |-- <ID of Container3>/
//      |-- ...
constexpr char CSI_DIR[] = "isolators/volume/csi";


std::string getContainerDir(
    const std::string& rootDir,
    const ContainerID& containerId);


std::string getVolumesPath(
    const std::string& rootDir,
    const ContainerID& containerId);

} // namespace paths {
} // namespace csi {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __VOLUME_CSI_ISOLATOR_PATHS_HPP__
