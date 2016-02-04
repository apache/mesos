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

#include <list>

#include <glog/logging.h>

#include <stout/path.hpp>

#include "slave/containerizer/mesos/provisioner/appc/paths.hpp"

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace appc {
namespace paths {

string getStagingDir(const string& storeDir)
{
  return path::join(storeDir, "staging");
}


string getImagesDir(const string& storeDir)
{
  return path::join(storeDir, "images");
}


string getImagePath(const string& storeDir, const string& imageId)
{
  return path::join(getImagesDir(storeDir), imageId);
}


string getImageRootfsPath(
    const string& storeDir,
    const string& imageId)
{
  return path::join(getImagePath(storeDir, imageId), "rootfs");
}


string getImageManifestPath(
    const string& storeDir,
    const string& imageId)
{
  return path::join(getImagePath(storeDir, imageId), "manifest");
}

} // namespace paths {
} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
