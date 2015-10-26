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

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace registry {

// Forward declarations.
class RegistryClientProcess;


class RegistryClient
{
public:
  /**
   * Encapsulates information about a file system layer.
   */
  struct FileSystemLayerInfo {
    // TODO(jojy): This string includes the checksum type also now. Need to
    // separate this into checksum method and checksum.
    const std::string checksumInfo;
    const std::string layerId;
  };

  /**
   * Encapsulates response of "GET Manifest" request.
   *
   * Reference: https://docs.docker.com/registry/spec/api
   */
  struct ManifestResponse {
    const std::string name;
    const std::string digest;
    const std::vector<FileSystemLayerInfo> fsLayerInfoList;
  };

  /**
   * Encapsulates auth credentials for the client sessions.
   * TODO(jojy): Secure heap to protect the credentials.
   */
  struct Credentials {
    /**
     * UserId for basic authentication.
     */
    const Option<std::string> userId;
    /**
     * Password for basic authentication.
     */
    const Option<std::string> password;
    /**
     * Account for fetching data from registry.
     */
    const Option<std::string> account;
  };

  /**
   * Factory method for creating RegistryClient objects.
   *
   * @param registryServer URL of docker registry server.
   * @param authServer URL of authorization server.
   * @param credentials credentials for client session (optional).
   * @return RegistryClient on Success.
   *         Error on failure.
   */
  static Try<process::Owned<RegistryClient>> create(
      const process::http::URL& registryServer,
      const process::http::URL& authServer,
      const Option<Credentials>& credentials);

  /**
   * Fetches manifest for a repository from the client's remote registry server.
   *
   * @param path path of the repository on the registry.
   * @param tag unique tag that identifies the repository. Will default to
   *    latest.
   * @param timeout Maximum time ater which the request will timeout and return
   *    a failure. Will default to RESPONSE_TIMEOUT.
   * @return JSON object on success.
   *         Failure on process failure.
   */
  process::Future<ManifestResponse> getManifest(
      const std::string& path,
      const Option<std::string>& tag,
      const Option<Duration>& timeout);

  /**
   * Fetches blob for a repository from the client's remote registry server.
   *
   * @param path path of the repository on the registry.
   * @param digest digest of the blob (from manifest).
   * @param filePath file path to store the fetched blob.
   * @param timeout Maximum time ater which the request will timeout and return
   *    a failure. Will default to RESPONSE_TIMEOUT.
   * @param maxSize Maximum size of the response thats acceptable. Will default
   *    to MAX_RESPONSE_SIZE.
   * @return size of downloaded blob on success.
   *         Failure in case of any errors.
   */
  process::Future<size_t> getBlob(
      const std::string& path,
      const Option<std::string>& digest,
      const Path& filePath,
      const Option<Duration>& timeout,
      const Option<size_t>& maxSize);

  ~RegistryClient();

private:
  RegistryClient(
    const process::http::URL& registryServer,
    const process::http::URL& authServer,
    const Option<Credentials>& credentials,
    const process::Owned<RegistryClientProcess>& process);

  static const Duration DEFAULT_MANIFEST_TIMEOUT_SECS;
  static const size_t DEFAULT_MANIFEST_MAXSIZE_BYTES;

  const process::http::URL registryServer_;
  const process::http::URL authServer_;
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
