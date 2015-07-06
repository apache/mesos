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

#include <stout/hashset.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "slave/containerizer/provisioner.hpp"

#include "slave/containerizer/provisioners/appc/provisioner.hpp"
#include "slave/containerizer/provisioners/docker/provisioner.hpp"

using namespace process;

using std::string;

namespace mesos {
namespace internal {
namespace slave {

Try<hashmap<Image::Type, Owned<Provisioner>>> Provisioner::create(
    const Flags& flags,
    Fetcher* fetcher)
{
  if (flags.provisioners.isNone()) {
    return hashmap<Image::Type, Owned<Provisioner>>();
  }

  hashmap<Image::Type,
          Try<Owned<Provisioner>>(*)(const Flags&, Fetcher*)> creators;

  // Register all supported creators.
  creators.put(Image::APPC, &appc::AppcProvisioner::create);
  creators.put(Image::DOCKER, &docker::DockerProvisioner::create);

  hashmap<Image::Type, Owned<Provisioner>> provisioners;

  // NOTE: Change in '--provisioners' flag may result in leaked rootfs
  // files on the disk but it's at least safe because files managed by
  // different provisioners are totally separated.
  foreach (const string& type,
           strings::tokenize(flags.provisioners.get(), ",")) {
     Image::Type imageType;
     if (!Image::Type_Parse(strings::upper(type), &imageType)) {
       return Error("Unknown provisioner '" + type + "'");
     }

     if (!creators.contains(imageType)) {
       return Error("Unsupported provisioner '" + type + "'");
     }

     Try<Owned<Provisioner>> provisioner = creators[imageType](flags, fetcher);
     if (provisioner.isError()) {
       return Error("Failed to create '" + stringify(imageType) +
                    "' provisioner: " + provisioner.error());
     }

     provisioners[imageType] = provisioner.get();
  }

  return provisioners;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
