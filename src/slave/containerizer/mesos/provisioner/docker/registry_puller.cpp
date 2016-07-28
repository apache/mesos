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

#include <glog/logging.h>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/http.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/rm.hpp>
#include <stout/os/write.hpp>

#include "common/command_utils.hpp"

#include "uri/schemes/docker.hpp"

#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"
#include "slave/containerizer/mesos/provisioner/docker/registry_puller.hpp"

namespace http = process::http;
namespace spec = docker::spec;

using std::list;
using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::Shared;

using process::defer;
using process::dispatch;
using process::spawn;
using process::wait;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class RegistryPullerProcess : public Process<RegistryPullerProcess>
{
public:
  RegistryPullerProcess(
      const string& _storeDir,
      const http::URL& _defaultRegistryUrl,
      const Shared<uri::Fetcher>& _fetcher);

  Future<vector<string>> pull(
      const spec::ImageReference& reference,
      const string& directory);

private:
  Future<vector<string>> _pull(
      const spec::ImageReference& reference,
      const string& directory);

  Future<vector<string>> __pull(
    const spec::ImageReference& reference,
    const string& directory,
    const spec::v2::ImageManifest& manifest,
    const hashset<string>& blobSums);

  Future<hashset<string>> fetchBlobs(
    const spec::ImageReference& reference,
    const string& directory,
    const spec::v2::ImageManifest& manifest);

  RegistryPullerProcess(const RegistryPullerProcess&) = delete;
  RegistryPullerProcess& operator=(const RegistryPullerProcess&) = delete;

  const string storeDir;

  // If the user does not specify the registry url in the image
  // reference, this registry url will be used as the default.
  const http::URL defaultRegistryUrl;

  Shared<uri::Fetcher> fetcher;
};


Try<Owned<Puller>> RegistryPuller::create(
    const Flags& flags,
    const Shared<uri::Fetcher>& fetcher)
{
  Try<http::URL> defaultRegistryUrl = http::URL::parse(flags.docker_registry);
  if (defaultRegistryUrl.isError()) {
    return Error(
        "Failed to parse the default Docker registry: " +
        defaultRegistryUrl.error());
  }

  VLOG(1) << "Creating registry puller with docker registry '"
          << flags.docker_registry << "'";

  Owned<RegistryPullerProcess> process(
      new RegistryPullerProcess(
          flags.docker_store_dir,
          defaultRegistryUrl.get(),
          fetcher));

  return Owned<Puller>(new RegistryPuller(process));
}


RegistryPuller::RegistryPuller(Owned<RegistryPullerProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


RegistryPuller::~RegistryPuller()
{
  terminate(process.get());
  wait(process.get());
}


Future<vector<string>> RegistryPuller::pull(
    const spec::ImageReference& reference,
    const string& directory)
{
  return dispatch(
      process.get(),
      &RegistryPullerProcess::pull,
      reference,
      directory);
}


RegistryPullerProcess::RegistryPullerProcess(
    const string& _storeDir,
    const http::URL& _defaultRegistryUrl,
    const Shared<uri::Fetcher>& _fetcher)
  : ProcessBase(process::ID::generate("docker-provisioner-registry_puller")),
    storeDir(_storeDir),
    defaultRegistryUrl(_defaultRegistryUrl),
    fetcher(_fetcher) {}


static spec::ImageReference normalize(
    const spec::ImageReference& _reference,
    const http::URL& defaultRegistryUrl)
{
  spec::ImageReference reference = _reference;

  // Determine which registry domain should be used.
  Option<string> registryDomain;

  if (_reference.has_registry()) {
    registryDomain = _reference.registry();
  } else {
    registryDomain = defaultRegistryUrl.domain.isSome()
      ? defaultRegistryUrl.domain.get()
      : Option<string>();
  }

  // Check if necessary to add 'library/' prefix for the case that the
  // registry is docker default 'https://registry-1.docker.io',
  // because docker official images locate in 'library/' directory.
  // For details, please see:
  // https://github.com/docker-library/official-images
  // https://github.com/docker/docker/blob/v1.10.2/reference/reference.go
  if (registryDomain.isSome() &&
      strings::contains(registryDomain.get(), "docker.io") &&
      !strings::contains(_reference.repository(), "/")) {
    const string repository = path::join("library", _reference.repository());

    reference.set_repository(repository);
  }

  return reference;
}


Future<vector<string>> RegistryPullerProcess::pull(
    const spec::ImageReference& _reference,
    const string& directory)
{
  spec::ImageReference reference = normalize(_reference, defaultRegistryUrl);

  URI manifestUri;
  if (reference.has_registry()) {
    Result<int> port = spec::getRegistryPort(reference.registry());
    if (port.isError()) {
      return Failure("Failed to get registry port: " + port.error());
    }

    Try<string> scheme = spec::getRegistryScheme(reference.registry());
    if (scheme.isError()) {
      return Failure("Failed to get registry scheme: " + scheme.error());
    }

    manifestUri = uri::docker::manifest(
        reference.repository(),
        (reference.has_tag() ? reference.tag() : "latest"),
        spec::getRegistryHost(reference.registry()),
        scheme.get(),
        port.isSome() ? port.get() : Option<int>());
  } else {
    const string registry = defaultRegistryUrl.domain.isSome()
      ? defaultRegistryUrl.domain.get()
      : stringify(defaultRegistryUrl.ip.get());

    const Option<int> port = defaultRegistryUrl.port.isSome()
      ? static_cast<int>(defaultRegistryUrl.port.get())
      : Option<int>();

    manifestUri = uri::docker::manifest(
        reference.repository(),
        (reference.has_tag() ? reference.tag() : "latest"),
        registry,
        defaultRegistryUrl.scheme,
        port);
  }

  VLOG(1) << "Pulling image '" << reference
          << "' from '" << manifestUri
          << "' to '" << directory << "'";

  return fetcher->fetch(manifestUri, directory)
    .then(defer(self(), &Self::_pull, reference, directory));
}


Future<vector<string>> RegistryPullerProcess::_pull(
    const spec::ImageReference& reference,
    const string& directory)
{
  Try<string> _manifest = os::read(path::join(directory, "manifest"));
  if (_manifest.isError()) {
    return Failure("Failed to read the manifest: " + _manifest.error());
  }

  Try<spec::v2::ImageManifest> manifest = spec::v2::parse(_manifest.get());
  if (manifest.isError()) {
    return Failure("Failed to parse the manifest: " + manifest.error());
  }

  VLOG(1) << "The manifest for image '" << reference << "' is '"
          << _manifest.get() << "'";

  // NOTE: This can be a CHECK (i.e., shouldn't happen). However, in
  // case docker has bugs, we return a Failure instead.
  if (manifest->fslayers_size() != manifest->history_size()) {
    return Failure("'fsLayers' and 'history' have different size in manifest");
  }

  return fetchBlobs(reference, directory, manifest.get())
    .then(defer(self(),
                &Self::__pull,
                reference,
                directory,
                manifest.get(),
                lambda::_1));
}


Future<vector<string>> RegistryPullerProcess::__pull(
    const spec::ImageReference& reference,
    const string& directory,
    const spec::v2::ImageManifest& manifest,
    const hashset<string>& blobSums)
{
  vector<string> layerIds;
  list<Future<Nothing>> futures;

  for (int i = 0; i < manifest.fslayers_size(); i++) {
    CHECK(manifest.history(i).has_v1());
    const spec::v1::ImageManifest& v1 = manifest.history(i).v1();
    const string& blobSum = manifest.fslayers(i).blobsum();

    // NOTE: We put parent layer ids in front because that's what the
    // provisioner backends assume.
    layerIds.insert(layerIds.begin(), v1.id());

    // Skip if the layer is already in the store.
    if (os::exists(paths::getImageLayerPath(storeDir, v1.id()))) {
      continue;
    }

    const string layerPath = path::join(directory, v1.id());
    const string tar = path::join(directory, blobSum);
    const string rootfs = paths::getImageLayerRootfsPath(layerPath);
    const string json = paths::getImageLayerManifestPath(layerPath);

    VLOG(1) << "Extracting layer tar ball '" << tar
            << " to rootfs '" << rootfs << "'";

    // NOTE: This will create 'layerPath' as well.
    Try<Nothing> mkdir = os::mkdir(rootfs, true);
    if (mkdir.isError()) {
      return Failure(
          "Failed to create rootfs directory '" + rootfs + "' "
          "for layer '" + v1.id() + "': " + mkdir.error());
    }

    Try<Nothing> write = os::write(json, stringify(JSON::protobuf(v1)));
    if (write.isError()) {
      return Failure(
          "Failed to save the layer manifest for layer '" +
          v1.id() + "': " + write.error());
    }

    futures.push_back(command::untar(Path(tar), Path(rootfs)));
  }

  return collect(futures)
    .then([=]() -> Future<vector<string>> {
      // Remove the tarballs after the extraction.
      foreach (const string& blobSum, blobSums) {
        const string tar = path::join(directory, blobSum);

        Try<Nothing> rm = os::rm(tar);
        if (rm.isError()) {
          return Failure(
              "Failed to remove '" + tar + "' "
              "after extraction: " + rm.error());
        }
      }

      return layerIds;
    });
}


Future<hashset<string>> RegistryPullerProcess::fetchBlobs(
    const spec::ImageReference& reference,
    const string& directory,
    const spec::v2::ImageManifest& manifest)
{
  // First, find all the blobs that need to be fetched.
  //
  // NOTE: There might exist duplicated blob sums in 'fsLayers'. We
  // just need to fetch one of them.
  hashset<string> blobSums;

  for (int i = 0; i < manifest.fslayers_size(); i++) {
    CHECK(manifest.history(i).has_v1());
    const spec::v1::ImageManifest& v1 = manifest.history(i).v1();

    // Check if the layer is in the store or not. If yes, skip the
    // unnecessary fetching.
    if (os::exists(paths::getImageLayerPath(storeDir, v1.id()))) {
      continue;
    }

    const string& blobSum = manifest.fslayers(i).blobsum();

    VLOG(1) << "Fetching blob '" << blobSum << "' for layer '"
            << v1.id() << "' of image '" << reference << "'";

    blobSums.insert(blobSum);
  }

  // Now, actually fetch the blobs.
  list<Future<Nothing>> futures;

  foreach (const string& blobSum, blobSums) {
    URI blobUri;

    if (reference.has_registry()) {
      Result<int> port = spec::getRegistryPort(reference.registry());
      if (port.isError()) {
        return Failure("Failed to get registry port: " + port.error());
      }

      Try<string> scheme = spec::getRegistryScheme(reference.registry());
      if (scheme.isError()) {
        return Failure("Failed to get registry scheme: " + scheme.error());
      }

      // If users want to use the registry specified in '--docker_image',
      // an URL scheme must be specified in '--docker_registry', because
      // there is no scheme allowed in docker image name.
      blobUri = uri::docker::blob(
          reference.repository(),
          blobSum,
          spec::getRegistryHost(reference.registry()),
          scheme.get(),
          port.isSome() ? port.get() : Option<int>());
    } else {
      const string registry = defaultRegistryUrl.domain.isSome()
        ? defaultRegistryUrl.domain.get()
        : stringify(defaultRegistryUrl.ip.get());

      const Option<int> port = defaultRegistryUrl.port.isSome()
        ? static_cast<int>(defaultRegistryUrl.port.get())
        : Option<int>();

      blobUri = uri::docker::blob(
          reference.repository(),
          blobSum,
          registry,
          defaultRegistryUrl.scheme,
          port);
    }

    futures.push_back(fetcher->fetch(blobUri, directory));
  }

  return collect(futures)
    .then([blobSums]() -> hashset<string> { return blobSums; });
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
