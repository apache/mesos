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

#include <mesos/secret/resolver.hpp>

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
      const Shared<uri::Fetcher>& _fetcher,
      SecretResolver* _secretResolver);

  Future<vector<string>> pull(
      const spec::ImageReference& reference,
      const string& directory,
      const string& backend,
      const Option<Secret>& config);

private:
  Future<vector<string>> _pull(
      const spec::ImageReference& reference,
      const string& directory,
      const string& backend,
      const Option<Secret::Value>& config = None());

  Future<vector<string>> __pull(
      const spec::ImageReference& reference,
      const string& directory,
      const string& backend,
      const Option<Secret::Value>& config);

  Future<vector<string>> ___pull(
    const spec::ImageReference& reference,
    const string& directory,
    const spec::v2::ImageManifest& manifest,
    const hashset<string>& blobSums,
    const string& backend);

  Future<hashset<string>> fetchBlobs(
    const spec::ImageReference& reference,
    const string& directory,
    const spec::v2::ImageManifest& manifest,
    const string& backend,
    const Option<Secret::Value>& config);

  RegistryPullerProcess(const RegistryPullerProcess&) = delete;
  RegistryPullerProcess& operator=(const RegistryPullerProcess&) = delete;

  const string storeDir;

  // If the user does not specify the registry url in the image
  // reference, this registry url will be used as the default.
  const http::URL defaultRegistryUrl;

  Shared<uri::Fetcher> fetcher;
  SecretResolver* secretResolver;
};


Try<Owned<Puller>> RegistryPuller::create(
    const Flags& flags,
    const Shared<uri::Fetcher>& fetcher,
    SecretResolver* secretResolver)
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
          fetcher,
          secretResolver));

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
    const string& directory,
    const string& backend,
    const Option<Secret>& config)
{
  return dispatch(
      process.get(),
      &RegistryPullerProcess::pull,
      reference,
      directory,
      backend,
      config);
}


RegistryPullerProcess::RegistryPullerProcess(
    const string& _storeDir,
    const http::URL& _defaultRegistryUrl,
    const Shared<uri::Fetcher>& _fetcher,
    SecretResolver* _secretResolver)
  : ProcessBase(process::ID::generate("docker-provisioner-registry-puller")),
    storeDir(_storeDir),
    defaultRegistryUrl(_defaultRegistryUrl),
    fetcher(_fetcher),
    secretResolver(_secretResolver) {}


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
    const spec::ImageReference& reference,
    const string& directory,
    const string& backend,
    const Option<Secret>& config)
{
  if (config.isNone()) {
    return _pull(reference, directory, backend);
  }

  return secretResolver->resolve(config.get())
    .then(defer(self(),
                &Self::_pull,
                reference,
                directory,
                backend,
                lambda::_1));
}


Future<vector<string>> RegistryPullerProcess::_pull(
    const spec::ImageReference& _reference,
    const string& directory,
    const string& backend,
    const Option<Secret::Value>& config)
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
        (reference.has_digest()
          ? reference.digest()
          : (reference.has_tag() ? reference.tag() : "latest")),
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
        (reference.has_digest()
          ? reference.digest()
          : (reference.has_tag() ? reference.tag() : "latest")),
        registry,
        defaultRegistryUrl.scheme,
        port);
  }

  VLOG(1) << "Pulling image '" << reference
          << "' from '" << manifestUri
          << "' to '" << directory << "'";

  return fetcher->fetch(
      manifestUri,
      directory,
      config.isSome() ? config->data() : Option<string>())
    .then(defer(self(), &Self::__pull, reference, directory, backend, config));
}


Future<vector<string>> RegistryPullerProcess::__pull(
    const spec::ImageReference& reference,
    const string& directory,
    const string& backend,
    const Option<Secret::Value>& config)
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

  return fetchBlobs(reference, directory, manifest.get(), backend, config)
    .then(defer(self(),
                &Self::___pull,
                reference,
                directory,
                manifest.get(),
                lambda::_1,
                backend));
}


Future<vector<string>> RegistryPullerProcess::___pull(
    const spec::ImageReference& reference,
    const string& directory,
    const spec::v2::ImageManifest& manifest,
    const hashset<string>& blobSums,
    const string& backend)
{
  // Docker reads the layer ids from the disk:
  // https://github.com/docker/docker/blob/v1.13.0/layer/filestore.go#L310
  //
  // So the layer ids should be unique and ordered alphabetically.
  // Then, docker loads each layer depending on the layer's
  // `parent-child` relationship:
  // https://github.com/docker/docker/blob/v1.13.0/layer/layer_store.go#L90
  //
  // In the registry puller, the vector of layer ids are collected
  // differently (not relying on finding the parent from the base
  // layer). All the layer ids are queued using the returned manifest
  // , and rely on the fact that the `v1Compatibility::id` from the
  // manifest are pre-sorted based on the `parent-child` relationship.
  //
  // Some self-built images may contain duplicate layers (e.g., [a, a,
  // b, b, b, c] from the manifest) which will cause failures for some
  // backends (e.g., Aufs). We should filter the layer ids to make
  // sure ids are unique.
  hashset<string> uniqueIds;
  vector<string> layerIds;
  vector<Future<Nothing>> futures;

  // The order of `fslayers` should be [child, parent, ...].
  //
  // The content in the parent will be overwritten by the child if
  // there is a conflict. Therefore, backends expect the following
  // order: [parent, child, ...].
  for (int i = 0; i < manifest.fslayers_size(); i++) {
    CHECK(manifest.history(i).has_v1());
    const spec::v1::ImageManifest& v1 = manifest.history(i).v1();
    const string& blobSum = manifest.fslayers(i).blobsum();

    // Skip duplicate layer ids.
    if (uniqueIds.contains(v1.id())) {
      continue;
    }

    // NOTE: We put parent layer ids in front because that's what the
    // provisioner backends assume.
    layerIds.insert(layerIds.begin(), v1.id());
    uniqueIds.insert(v1.id());

    // Skip if the layer is already in the store.
    if (os::exists(
        paths::getImageLayerRootfsPath(storeDir, v1.id(), backend))) {
      continue;
    }

    const string layerPath = path::join(directory, v1.id());
    const string tar = path::join(directory, blobSum);
    const string rootfs = paths::getImageLayerRootfsPath(layerPath, backend);
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
    const spec::v2::ImageManifest& manifest,
    const string& backend,
    const Option<Secret::Value>& config)
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
    if (os::exists(
        paths::getImageLayerRootfsPath(storeDir, v1.id(), backend))) {
      continue;
    }

    const string& blobSum = manifest.fslayers(i).blobsum();

    VLOG(1) << "Fetching blob '" << blobSum << "' for layer '"
            << v1.id() << "' of image '" << reference << "'";

    blobSums.insert(blobSum);
  }

  // Now, actually fetch the blobs.
  vector<Future<Nothing>> futures;

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

    futures.push_back(fetcher->fetch(
        blobUri,
        directory,
        config.isSome() ? config->data() : Option<string>()));
  }

  return collect(futures)
    .then([blobSums]() -> hashset<string> { return blobSums; });
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
