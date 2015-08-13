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

#ifndef __MESOS_APPC_STORE__
#define __MESOS_APPC_STORE__

#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/try.hpp>

#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

// Forward declaration.
class StoreProcess;


// Provides the provisioner with access to locally stored / cached Appc images.
class Store
{
public:
  // Defines an image in the store (which has passed validation).
  struct Image
  {
    Image(
        const AppcImageManifest& _manifest,
        const std::string& _id,
        const std::string& _path)
      : manifest(_manifest), id(_id), path(_path) {}

    const AppcImageManifest manifest;

    // Image ID of the format "sha512-value" where "value" is the hex
    // encoded string of the sha512 digest of the uncompressed tar file
    // of the image.
    const std::string id;

    // Absolute path of the extracted image.
    const std::string path;
  };

  static Try<process::Owned<Store>> create(const Flags& flags);

  ~Store();

  process::Future<Nothing> recover();

  // Get all images matched by name.
  process::Future<std::vector<Image>> get(const std::string& name);

  // TODO(xujyan): Implement a put() method that fetches an image into
  // the store. i.e.,
  // process::Future<StoredImage> put(const std::string& uri);

private:
  Store(process::Owned<StoreProcess> process);

  Store(const Store&); // Not copyable.
  Store& operator=(const Store&); // Not assignable.

  process::Owned<StoreProcess> process;
};

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_APPC_STORE__
