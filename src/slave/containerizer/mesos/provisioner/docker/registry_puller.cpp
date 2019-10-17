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
#include <stout/os/rename.hpp>
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

  Future<Image> pull(
      const spec::ImageReference& reference,
      const string& directory,
      const string& backend,
      const Option<Secret>& config);

private:
  Future<Image> _pull(
      const spec::ImageReference& reference,
      const string& directory,
      const string& backend,
      const Option<Secret::Value>& config = None());

  Future<Image> __pull(
      const spec::ImageReference& reference,
      const string& directory,
      const string& backend,
      const Option<Secret::Value>& config);

  Future<Image> ___pull(
    const spec::ImageReference& reference,
    const string& directory,
    const spec::v2::ImageManifest& manifest,
    const hashset<string>& blobSums,
    const string& backend);

  Future<Image> ____pull(
    const spec::ImageReference& reference,
    const string& directory,
    const spec::v2_2::ImageManifest& manifest,
    const hashset<string>& digests,
    const string& backend);

  Future<hashset<string>> fetchBlobs(
    const spec::ImageReference& normalizedRef,
    const string& directory,
    const spec::v2::ImageManifest& manifest,
    const string& backend,
    const Option<Secret::Value>& config);

  Future<hashset<string>> fetchBlobs(
      const spec::ImageReference& normalizedRef,
      const string& directory,
      const spec::v2_2::ImageManifest& manifest,
      const string& backend,
      const Option<Secret::Value>& config);

  Future<hashset<string>> fetchBlobs(
      const spec::ImageReference& normalizedRef,
      const string& directory,
      const hashset<string>& digests,
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


Future<Image> RegistryPuller::pull(
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


Future<Image> RegistryPullerProcess::pull(
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


Future<Image> RegistryPullerProcess::_pull(
    const spec::ImageReference& reference,
    const string& directory,
    const string& backend,
    const Option<Secret::Value>& config)
{
  spec::ImageReference normalizedRef = normalize(reference, defaultRegistryUrl);

  URI manifestUri;
  if (normalizedRef.has_registry()) {
    Result<int> port = spec::getRegistryPort(normalizedRef.registry());
    if (port.isError()) {
      return Failure("Failed to get registry port: " + port.error());
    }

    Try<string> scheme = spec::getRegistryScheme(normalizedRef.registry());
    if (scheme.isError()) {
      return Failure("Failed to get registry scheme: " + scheme.error());
    }

    manifestUri = uri::docker::manifest(
        normalizedRef.repository(),
        (normalizedRef.has_digest()
          ? normalizedRef.digest()
          : (normalizedRef.has_tag() ? normalizedRef.tag() : "latest")),
        spec::getRegistryHost(normalizedRef.registry()),
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
        normalizedRef.repository(),
        (normalizedRef.has_digest()
          ? normalizedRef.digest()
          : (normalizedRef.has_tag() ? normalizedRef.tag() : "latest")),
        registry,
        defaultRegistryUrl.scheme,
        port);
  }

  LOG(INFO) << "Fetching manifest from '" << manifestUri << "' to '"
            << directory << "' for image '" << normalizedRef << "'";

  // Pass the original 'reference' along to subsequent methods
  // because metadata manager may already has this reference in
  // cache. This is necessary to ensure the backward compatibility
  // after upgrading to the version including MESOS-9675 for
  // docker manifest v2 schema2 support.
  return fetcher->fetch(
      manifestUri,
      directory,
      config.isSome() ? config->data() : Option<string>())
    .then(defer(self(), &Self::__pull, reference, directory, backend, config));
}


Future<Image> RegistryPullerProcess::__pull(
    const spec::ImageReference& reference,
    const string& directory,
    const string& backend,
    const Option<Secret::Value>& config)
{
  Try<string> _manifest = os::read(path::join(directory, "manifest"));
  if (_manifest.isError()) {
    return Failure("Failed to read the manifest: " + _manifest.error());
  }

  VLOG(1) << "The manifest for image '" << reference << "' is '"
          << _manifest.get() << "'";

  // To ensure backward compatibility in upgrade case for docker
  // manifest v2 schema2 support, it is unavoidable to call
  // 'normalize()' twice because some existing image may already
  // be cached by metadata manager before upgrade and now metadata
  // persists the cache from the image information constructed at
  // registry puller. Please see MESOS-9675 for details.
  spec::ImageReference normalizedRef = normalize(reference, defaultRegistryUrl);

  Try<JSON::Object> json = JSON::parse<JSON::Object>(_manifest.get());
  if (json.isError()) {
    return Failure("Failed to parse the manifest JSON: " + json.error());
  }

  Result<JSON::Number> schemaVersion = json->at<JSON::Number>("schemaVersion");
  if (schemaVersion.isError()) {
    return Failure(
        "Failed to find manifest schema version: " + schemaVersion.error());
  }

  if (schemaVersion.isSome() && schemaVersion->as<int>() == 2) {
    Try<spec::v2_2::ImageManifest> manifest = spec::v2_2::parse(json.get());
    if (manifest.isError()) {
      return Failure("Failed to parse the manifest: " + manifest.error());
    }

    return fetchBlobs(normalizedRef, directory, manifest.get(), backend, config)
      .then(defer(self(),
                  &Self::____pull,
                  reference,
                  directory,
                  manifest.get(),
                  lambda::_1,
                  backend));
  }

  // By default treat the manifest format as schema 1.
  Try<spec::v2::ImageManifest> manifest = spec::v2::parse(json.get());
  if (manifest.isError()) {
    return Failure("Failed to parse the manifest: " + manifest.error());
  }

  // NOTE: This can be a CHECK (i.e., shouldn't happen). However, in
  // case docker has bugs, we return a Failure instead.
  if (manifest->fslayers_size() != manifest->history_size()) {
    return Failure("'fsLayers' and 'history' have different size in manifest");
  }

  return fetchBlobs(normalizedRef, directory, manifest.get(), backend, config)
    .then(defer(self(),
                &Self::___pull,
                reference,
                directory,
                manifest.get(),
                lambda::_1,
                backend));
}


Future<Image> RegistryPullerProcess::___pull(
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

  LOG(INFO) << "Extracting layers to '" << directory
            << "' for image '" << reference << "'";

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

    VLOG(1) << "Extracting layer tar ball '" << tar << " to rootfs '"
            << rootfs << "' for image '" << reference << "'";

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
    .then([=]() -> Future<Image> {
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

      Image image;
      image.mutable_reference()->CopyFrom(reference);
      foreach (const string& layerId, layerIds) {
        image.add_layer_ids(layerId);
      }

      return image;
    });
}


Future<Image> RegistryPullerProcess::____pull(
    const spec::ImageReference& reference,
    const string& directory,
    const spec::v2_2::ImageManifest& manifest,
    const hashset<string>& digests,
    const string& backend)
{
  hashset<string> uniqueIds;
  vector<string> layerIds;
  vector<Future<Nothing>> futures;

  LOG(INFO) << "Extracting layers to '" << directory
            << "' for image '" << reference << "'";

  for (int i = 0; i < manifest.layers_size(); i++) {
    const string& digest = manifest.layers(i).digest();
    if (uniqueIds.contains(digest)) {
      continue;
    }

    layerIds.push_back(digest);
    uniqueIds.insert(digest);

    // Skip if the layer is already in the store.
    if (os::exists(paths::getImageLayerRootfsPath(storeDir, digest, backend))) {
      continue;
    }

    const string layerPath = path::join(directory, digest);
    const string originalTar = path::join(directory, digest);
    const string tar = path::join(directory, digest + "-archive");
    const string rootfs = paths::getImageLayerRootfsPath(layerPath, backend);

    VLOG(1) << "Moving layer tar ball '" << originalTar << "' to '"
            << tar << "' for image '" << reference << "'";

    // Move layer tar ball to use its name for the extracted layer directory.
    Try<Nothing> rename = os::rename(originalTar, tar);
    if (rename.isError()) {
      return Failure(
          "Failed to move the layer tar ball from '" + originalTar +
          "' to '" + tar + "': " + rename.error());
    }

    VLOG(1) << "Extracting layer tar ball '" << tar << "' to rootfs '"
            << rootfs << "' for image '" << reference << "'";

    // NOTE: This will create 'layerPath' as well.
    Try<Nothing> mkdir = os::mkdir(rootfs, true);
    if (mkdir.isError()) {
      return Failure(
          "Failed to create rootfs directory '" + rootfs + "' "
          "for layer '" + digest + "': " + mkdir.error());
    }

    futures.push_back(command::untar(Path(tar), Path(rootfs)));
  }

  return collect(futures)
    .then([=]() -> Future<Image> {
      // Remove the tarballs after the extraction.
      foreach (const string& digest, digests) {
        // Skip if the digest represents the image manifest config.
        if (digest == manifest.config().digest()) {
          continue;
        }

        const string tar = path::join(directory, digest + "-archive");
        Try<Nothing> rm = os::rm(tar);
        if (rm.isError()) {
          return Failure(
              "Failed to remove '" + tar + "' after extraction: " + rm.error());
        }
      }

      Image image;
      image.set_config_digest(manifest.config().digest());
      image.mutable_reference()->CopyFrom(reference);
      foreach (const string& layerId, layerIds) {
        image.add_layer_ids(layerId);
      }

      return image;
    });
}


Future<hashset<string>> RegistryPullerProcess::fetchBlobs(
    const spec::ImageReference& normalizedRef,
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

  LOG(INFO) << "Fetching blobs to '" << directory << "' for image '"
            << normalizedRef << "'";

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

    VLOG(1) << "Fetching blob '" << blobSum << "' for layer '" << v1.id()
            << "' of image '" << normalizedRef << "' to '" << directory << "'";

    blobSums.insert(blobSum);
  }

  return fetchBlobs(normalizedRef, directory, blobSums, backend, config);
}


Future<hashset<string>> RegistryPullerProcess::fetchBlobs(
    const spec::ImageReference& normalizedRef,
    const string& directory,
    const spec::v2_2::ImageManifest& manifest,
    const string& backend,
    const Option<Secret::Value>& config)
{
  // First, find all the blobs that need to be fetched.
  //
  // NOTE: There might exist duplicated digests in 'layers'. We
  // just need to fetch one of them.
  hashset<string> digests;

  const string& configDigest = manifest.config().digest();
  if (!os::exists(paths::getImageLayerPath(storeDir, configDigest))) {
    LOG(INFO) << "Fetching config '" << configDigest << "' to '" << directory
              << "' for image '" << normalizedRef << "'";

    digests.insert(configDigest);
  }

  LOG(INFO) << "Fetching layers to '" << directory << "' for image '"
            << normalizedRef << "'";

  for (int i = 0; i < manifest.layers_size(); i++) {
    const string& digest = manifest.layers(i).digest();

    // Check if the layer is in the store or not. If yes, skip the unnecessary
    // fetching.
    if (os::exists(paths::getImageLayerRootfsPath(storeDir, digest, backend))) {
      continue;
    }

    VLOG(1) << "Fetching layer '" << digest << "' to '" << directory
            << "' for image '" << normalizedRef << "'";

    digests.insert(digest);
  }

  return fetchBlobs(normalizedRef, directory, digests, backend, config);
}


Future<hashset<string>> RegistryPullerProcess::fetchBlobs(
    const spec::ImageReference& normalizedRef,
    const string& directory,
    const hashset<string>& digests,
    const string& backend,
    const Option<Secret::Value>& config)
{
  vector<Future<Nothing>> futures;

  foreach (const string& digest, digests) {
    URI blobUri;

    if (normalizedRef.has_registry()) {
      Result<int> port = spec::getRegistryPort(normalizedRef.registry());
      if (port.isError()) {
        return Failure("Failed to get registry port: " + port.error());
      }

      Try<string> scheme = spec::getRegistryScheme(normalizedRef.registry());
      if (scheme.isError()) {
        return Failure("Failed to get registry scheme: " + scheme.error());
      }

      // If users want to use the registry specified in '--docker_image',
      // an URL scheme must be specified in '--docker_registry', because
      // there is no scheme allowed in docker image name.
      blobUri = uri::docker::blob(
          normalizedRef.repository(),
          digest,
          spec::getRegistryHost(normalizedRef.registry()),
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
          normalizedRef.repository(),
          digest,
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
    .then([digests]() -> hashset<string> { return digests; });
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
