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

#include <mesos/type_utils.hpp>

#include <stout/path.hpp>
#include <stout/stringify.hpp>

#include "slave/containerizer/mesos/isolators/volume/csi/paths.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace csi {
namespace paths {

string getContainerDir(const string& rootDir, const ContainerID& containerId)
{
  return path::join(rootDir, stringify(containerId));
}


string getVolumesPath(const string& rootDir, const ContainerID& containerId)
{
  return path::join(getContainerDir(rootDir, containerId), "volumes");
}

} // namespace paths {
} // namespace csi {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
