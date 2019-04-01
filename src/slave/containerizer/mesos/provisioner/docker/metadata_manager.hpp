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

#ifndef __PROVISIONER_DOCKER_METADATA_MANAGER_HPP__
#define __PROVISIONER_DOCKER_METADATA_MANAGER_HPP__

#include <list>
#include <string>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <mesos/docker/spec.hpp>

#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

#include "slave/containerizer/mesos/provisioner/docker/message.hpp"

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward Declaration.
class MetadataManagerProcess;

/**
 * The MetadataManager tracks the Docker images cached by the
 * provisioner that are stored on disk. It keeps track of the layers
 * that Docker images are composed of and recovers Image objects
 * upon initialization by checking for dependent layers stored on disk.
 * Currently, image layers are stored indefinitely, with no garbage
 * collection of unreferenced image layers.
 */
class MetadataManager
{
public:
  static Try<process::Owned<MetadataManager>> create(const Flags& flags);

  ~MetadataManager();

  /**
   * Recover all stored Image and its layer references.
   */
  process::Future<Nothing> recover();

  /**
   * Put the image in metadata manager and persist the reference
   * store state to disk.
   */
  process::Future<Image> put(const Image& image);

  /**
   * Retrieve Image based on image reference if it is among the Images
   * stored in memory.
   *
   * @param reference the reference of the Docker image to retrieve
   * @param cached the flag whether pull Docker image forcelly from remote
   *               registry or local repo.
   */
  process::Future<Option<Image>> get(
      const ::docker::spec::ImageReference& reference,
      bool cached);

  /**
   * Prune images from the metadata manager by comparing
   * existing images with active images in use. This function will
   * remove all images not used anymore, and return the list of
   * layers which are still referenced. The caller should
   * ensure such layers are kept in best effort.
   *
   * @param excludedImages all images to exclude from pruning.
   * @return a list of all layers still refered.
   */
  process::Future<hashset<std::string>> prune(
      const std::vector<::docker::spec::ImageReference>& excludedImages);

private:
  explicit MetadataManager(process::Owned<MetadataManagerProcess> process);

  MetadataManager(const MetadataManager&); // Not copyable.
  MetadataManager& operator=(const MetadataManager&); // Not assignable.

  process::Owned<MetadataManagerProcess> process;
};


} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_DOCKER_METADATA_MANAGER_HPP__
