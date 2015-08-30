/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <list>

#include <glog/logging.h>

#include <mesos/type_utils.hpp>

#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/stat.hpp>

#include "slave/containerizer/provisioners/appc/paths.hpp"
#include "slave/paths.hpp"

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


string getImageRootfsPath(const string& imagePath)
{
  return path::join(imagePath, "rootfs");
}


string getImageManifestPath(
    const string& storeDir,
    const string& imageId)
{
  return path::join(getImagePath(storeDir, imageId), "manifest");
}


string getImageManifestPath(const string& imagePath)
{
  return path::join(imagePath, "manifest");
}


// Internal helpers for traversing the directory hierarchy.
static string getContainersDir(const string& provisionerDir)
{
  return path::join(provisionerDir, "containers");
}


static string getContainerDir(
    const string& containersDir,
    const ContainerID& containerId)
{
  return path::join(containersDir, containerId.value());
}


static string getBackendsDir(const string& containerDir)
{
  return path::join(containerDir, "backends");
}


static string getBackendDir(const string& backendsDir, const string& backend)
{
  return path::join(backendsDir, backend);
}


static string getRootfsesDir(const string& backendDir)
{
  return path::join(backendDir, "rootfses");
}


static string getRootfsDir(const string& rootfsesDir, const string& roofsId)
{
  return path::join(rootfsesDir, roofsId);
}


string getContainerRootfsDir(
    const string& provisionerDir,
    const ContainerID& containerId,
    const string& backend,
    const string& rootfsId)
{
  return getRootfsDir(
      getRootfsesDir(
          getBackendDir(
              getBackendsDir(
                  getContainerDir(
                      getContainersDir(provisionerDir),
                      containerId)),
              backend)),
      rootfsId);
}


Try<hashmap<ContainerID, string>> listContainers(
    const string& provisionerDir)
{
  hashmap<ContainerID, string> results;

  string containersDir = getContainersDir(provisionerDir);
  if (!os::exists(containersDir)) {
    // No container has been created yet.
    return results;
  }

  Try<list<string>> containerIds = os::ls(containersDir);
  if (containerIds.isError()) {
    return Error("Unable to list the containers directory: " +
                 containerIds.error());
  }

  foreach (const string& entry, containerIds.get()) {
    string containerPath = path::join(containersDir, entry);

    if (!os::stat::isdir(containerPath)) {
      LOG(WARNING) << "Ignoring unexpected container entry at: "
                   << containerPath;
      continue;
    }

    ContainerID containerId;
    containerId.set_value(entry);
    results.put(containerId, containerPath);
  }

  return results;
}


Try<hashmap<string, hashmap<string, string>>> listContainerRootfses(
    const string& provisionerDir,
    const ContainerID& containerId)
{
  hashmap<string, hashmap<string, string>> results;

  string backendsDir = getBackendsDir(
      getContainerDir(
          getContainersDir(provisionerDir),
          containerId));

  Try<list<string>> backends = os::ls(backendsDir);
  if (backends.isError()) {
    return Error("Unable to list the container directory: " + backends.error());
  }

  foreach (const string& backend, backends.get()) {
    string backendDir = getBackendDir(backendsDir, backend);
    if (!os::stat::isdir(backendDir)) {
      LOG(WARNING) << "Ignoring unexpected backend entry at: " << backendDir;
      continue;
    }

    Try<list<string>> rootfses = os::ls(getRootfsesDir(backendDir));
    if (rootfses.isError()) {
      return Error("Unable to list the backend directory: " + rootfses.error());
    }

    hashmap<string, string> backendResults;

    foreach (const string& rootfsId, rootfses.get()) {
      string rootfs = getRootfsDir(getRootfsesDir(backendDir), rootfsId);

      if (!os::stat::isdir(rootfs)) {
        LOG(WARNING) << "Ignoring unexpected rootfs entry at: " << backendDir;
        continue;
      }

      backendResults.put(rootfsId, rootfs);
    }

    if (backendResults.empty()) {
      LOG(WARNING) << "Ignoring a backend directory with no rootfs in it: "
                   << backendDir;
      continue;
    }

    // The rootfs directory has passed validation.
    results.put(backend, backendResults);
  }

  return results;
}

} // namespace paths {
} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
