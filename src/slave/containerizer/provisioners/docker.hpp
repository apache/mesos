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

#ifndef __MESOS_DOCKER__
#define __MESOS_DOCKER__

#include <list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/shared.hpp>

#include <mesos/resources.hpp>

#include "slave/containerizer/provisioner.hpp"
#include "slave/containerizer/provisioners/backend.hpp"

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward declaration.
class Store;

struct DockerLayer {
  DockerLayer(
      const std::string& hash,
      const JSON::Object& manifest,
      const std::string& path,
      const std::string& version,
      const Option<process::Shared<DockerLayer>> parent)
    : hash(hash),
      manifest(manifest),
      path(path),
      version(version),
      parent(parent) {}

  DockerLayer() {}

  std::string hash;
  JSON::Object manifest;
  std::string path;
  std::string version;
  Option<process::Shared<DockerLayer>> parent;
};


struct DockerImage
{
  DockerImage(
      const std::string& name,
      const Option<process::Shared<DockerLayer>>& layer)
    : name(name), layer(layer) {}

  static Try<std::pair<std::string, std::string>> parseTag(
      const std::string& name)
  {
    std::size_t found = name.find_last_of(':');
    if (found == std::string::npos) {
      return make_pair(name, "latest");
    }
    return make_pair(name.substr(0, found), name.substr(found + 1));
  }

  DockerImage() {}

  std::string name;
  Option<process::Shared<DockerLayer>> layer;
};

// Forward declaration.
class DockerProvisionerProcess;

class DockerProvisioner : public Provisioner
{
public:
  static Try<process::Owned<Provisioner>> create(
      const Flags& flags,
      Fetcher* fetcher);

  virtual ~DockerProvisioner();

  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans);

  virtual process::Future<std::string> provision(
      const ContainerID& containerId,
      const Image& image,
      const std::string& sandbox);

  virtual process::Future<bool> destroy(const ContainerID& containerId);

private:
  explicit DockerProvisioner(process::Owned<DockerProvisionerProcess> process);
  DockerProvisioner(const DockerProvisioner&); // Not copyable.
  DockerProvisioner& operator=(const DockerProvisioner&); // Not assignable.

  process::Owned<DockerProvisionerProcess> process;
};


class DockerProvisionerProcess :
  public process::Process<DockerProvisionerProcess>
{
public:
  static Try<process::Owned<DockerProvisionerProcess>> create(
      const Flags& flags,
      Fetcher* fetcher);

  process::Future<Nothing> recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans);

  process::Future<std::string> provision(
      const ContainerID& containerId,
      const Image& image,
      const std::string& sandbox);

  process::Future<bool> destroy(const ContainerID& containerId);

private:
  DockerProvisionerProcess(
      const Flags& flags,
      const process::Owned<Store>& store,
      const process::Owned<mesos::internal::slave::Backend>& backend);

  process::Future<std::string> _provision(
      const ContainerID& containerId,
      const DockerImage& image);

  process::Future<DockerImage> fetch(
      const std::string& name,
      const std::string& sandbox);

  const Flags flags;

  process::Owned<Store> store;
  process::Owned<mesos::internal::slave::Backend> backend;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_DOCKER__
