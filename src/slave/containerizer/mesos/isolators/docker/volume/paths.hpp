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

#ifndef __ISOLATOR_VOLUME_PATHS_HPP__
#define __ISOLATOR_VOLUME_PATHS_HPP__

#include <string>

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace volume {
namespace paths {

// We checkpoint the information about docker volumes that each
// container uses. The layout is as follows:
//   <docker_volume_checkpoint_dir> ('--docker_volume_checkpoint_dir' flag)
//      |-- <ID of Container1>/
//      |      |-- volumes
//      |-- <ID of Container2>/
//      |      |-- volumes
//      |-- <ID of Container3>/
//      |-- ...

std::string getContainerDir(
    const std::string& rootDir,
    const std::string& containerId);


std::string getVolumesPath(
    const std::string& rootDir,
    const std::string& containerId);

} // namespace paths {
} // namespace volume {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __ISOLATOR_VOLUME_PATHS_HPP__
