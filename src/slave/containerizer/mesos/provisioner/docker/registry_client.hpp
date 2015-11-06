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

#ifndef __PROVISIONER_DOCKER_REGISTRY_CLIENT_HPP__
#define __PROVISIONER_DOCKER_REGISTRY_CLIENT_HPP__

#include <string>
#include <vector>

#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/path.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include "slave/containerizer/mesos/provisioner/docker/message.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace registry {

// Forward declarations.
class RegistryClientProcess;


// TODO(bmahler): Replace these with the existing protobuf counterparts.


struct FileSystemLayerInfo
{
  // TODO(jojy): This includes both the checksum and
  // the type of the checksum; separate these.
  const std::string checksumInfo;
  const std::string layerId;
};


/**
 * Response for a "GET Manifest" request.
 *
 * Reference:
 * https://github.com/docker/distribution/blob/master/docs/spec/manifest-v2-1.md
 */
struct Manifest
{
  static Try<Manifest> create(const std::string& jsonString);

  const std::string name;
  const std::vector<FileSystemLayerInfo> fsLayerInfos;
};


/**
 * Authentication credentials for the client sessions.
 */
struct Credentials
{
  const Option<std::string> userId;
  const Option<std::string> password;
  const Option<std::string> account;
};


class RegistryClient
{
public:
  /**
   * Factory method for creating RegistryClient objects.
   *
   * @param registryServer URL of docker registry server.
   * @param authorizationServer URL of authorization server.
   * @param credentials credentials for client session (optional).
   * @return RegistryClient on Success.
   *         Error on failure.
   */
  static Try<process::Owned<RegistryClient>> create(
      const process::http::URL& registryServer,
      const process::http::URL& authorizationServer,
      const Option<Credentials>& credentials);

  /**
   * Fetches manifest for a repository from the client's remote registry server.
   *
   * @param imageName Image information(Name, tag).
   * @return Manifest on success.
   *         Failure on process failure.
   */
  process::Future<Manifest> getManifest(
      const Image::Name& imageName);


  /**
   * Fetches blob for a repository from the client's remote registry server.
   *
   * @param path path of the repository on the registry.
   * @param digest digest of the blob (from manifest).
   * @param filePath file path to store the fetched blob.
   * @param maxSize Maximum size of the response thats acceptable. Will default
   *    to MAX_RESPONSE_SIZE.
   * @return size of downloaded blob on success.
   *         Failure in case of any errors.
   */
  process::Future<size_t> getBlob(
      const std::string& path,
      const Option<std::string>& digest,
      const Path& filePath);

  ~RegistryClient();

private:
  RegistryClient(
    const process::http::URL& registryServer,
    const process::http::URL& authorizationServer,
    const Option<Credentials>& credentials,
    const process::Owned<RegistryClientProcess>& process);

  const process::http::URL registryServer_;
  const process::http::URL authorizationServer_;
  const Option<Credentials> credentials_;
  process::Owned<RegistryClientProcess> process_;

  RegistryClient(const RegistryClient&) = delete;
  RegistryClient& operator=(const RegistryClient&) = delete;
};

} // namespace registry {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_DOCKER_REGISTRY_CLIENT_HPP__
