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

#include "slave/paths.hpp"

#include "slave/containerizer/provisioner/paths.hpp"

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace slave {
namespace provisioner {
namespace paths {

static string getContainersDir(const string& provisionerDir)
{
  return path::join(provisionerDir, "containers");
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


string getContainerDir(
    const string& provisionerDir,
    const ContainerID& containerId)
{
  return path::join(getContainersDir(provisionerDir), containerId.value());
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
                      provisionerDir,
                      containerId)),
              backend)),
      rootfsId);
}


Try<hashset<ContainerID>> listContainers(
    const string& provisionerDir)
{
  hashset<ContainerID> results;

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
    results.insert(containerId);
  }

  return results;
}


Try<hashmap<string, hashset<string>>> listContainerRootfses(
    const string& provisionerDir,
    const ContainerID& containerId)
{
  hashmap<string, hashset<string>> results;

  string backendsDir = getBackendsDir(
      getContainerDir(
          provisionerDir,
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

    hashset<string> backendResults;

    foreach (const string& rootfsId, rootfses.get()) {
      string rootfs = getRootfsDir(getRootfsesDir(backendDir), rootfsId);

      if (!os::stat::isdir(rootfs)) {
        LOG(WARNING) << "Ignoring unexpected rootfs entry at: " << backendDir;
        continue;
      }

      backendResults.insert(rootfsId);
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
} // namespace provisioner {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
