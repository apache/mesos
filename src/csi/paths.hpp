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

#ifndef __CSI_PATHS_HPP__
#define __CSI_PATHS_HPP__

#include <list>
#include <string>

#include <mesos/mesos.hpp>

#include <stout/try.hpp>

namespace mesos {
namespace csi {
namespace paths {

// The file system layout is as follows:
//
//   root (<work_dir>/csi/)
//   |-- <type>
//       |-- <name>
//           |-- containers
//           |   |-- <container_id>
//           |       |-- container.info
//           |       |-- endpoint (symlink to /tmp/mesos-csi-XXXXXX)
//           |           |-- endpoint.sock
//           |-- volumes
//           |   |-- <volume_id>
//           |        |-- volume.state
//           |-- mounts
//               |-- <volume_id>
//                    |- staging (staging mount point)
//                    |- target (mount point)


struct ContainerPath
{
  std::string type;
  std::string name;
  ContainerID containerId;
};


struct VolumePath
{
  std::string type;
  std::string name;
  std::string volumeId;
};


Try<std::list<std::string>> getContainerPaths(
    const std::string& rootDir,
    const std::string& type,
    const std::string& name);


std::string getContainerPath(
    const std::string& rootDir,
    const std::string& type,
    const std::string& name,
    const ContainerID& containerId);


Try<ContainerPath> parseContainerPath(
    const std::string& rootDir,
    const std::string& dir);


std::string getContainerInfoPath(
    const std::string& rootDir,
    const std::string& type,
    const std::string& name,
    const ContainerID& containerId);


std::string getEndpointDirSymlinkPath(
    const std::string& rootDir,
    const std::string& type,
    const std::string& name,
    const ContainerID& containerId);


// Returns the resolved path to the endpoint socket, even if the socket
// file itself does not exist. Creates and symlinks the endpoint
// directory if necessary.
Try<std::string> getEndpointSocketPath(
    const std::string& rootDir,
    const std::string& type,
    const std::string& name,
    const ContainerID& containerId);


Try<std::list<std::string>> getVolumePaths(
    const std::string& rootDir,
    const std::string& type,
    const std::string& name);


std::string getVolumePath(
    const std::string& rootDir,
    const std::string& type,
    const std::string& name,
    const std::string& volumeId);


Try<VolumePath> parseVolumePath(
    const std::string& rootDir,
    const std::string& dir);


std::string getVolumeStatePath(
    const std::string& rootDir,
    const std::string& type,
    const std::string& name,
    const std::string& volumeId);


std::string getMountRootDir(
    const std::string& rootDir,
    const std::string& type,
    const std::string& name);


Try<std::list<std::string>> getMountPaths(
    const std::string& mountRootDir);


std::string getMountPath(
    const std::string& mountRootDir,
    const std::string& volumeId);


Try<std::string> parseMountPath(
    const std::string& mountRootDir,
    const std::string& dir);


std::string getMountStagingPath(
    const std::string& mountRootDir,
    const std::string& volumeId);


std::string getMountTargetPath(
    const std::string& mountRootDir,
    const std::string& volumeId);

} // namespace paths {
} // namespace csi {
} // namespace mesos {

#endif // __CSI_PATHS_HPP__
