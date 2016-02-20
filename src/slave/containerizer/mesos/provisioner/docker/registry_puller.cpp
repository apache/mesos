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

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>

#include <stout/os/mkdir.hpp>
#include <stout/os/rm.hpp>

#include "common/command_utils.hpp"

#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"
#include "slave/containerizer/mesos/provisioner/docker/registry_client.hpp"
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
using process::Promise;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

using RegistryClient = registry::RegistryClient;


class RegistryPullerProcess : public Process<RegistryPullerProcess>
{
public:
  static Try<Owned<RegistryPullerProcess>> create(const Flags& flags);

  process::Future<vector<string>> pull(
      const spec::ImageReference& reference,
      const string& directory);

private:
  RegistryPullerProcess(
      const Owned<RegistryClient>& registry,
      const Duration& timeout);

  Future<vector<string>> downloadLayers(
      const spec::v2::ImageManifest& manifest,
      const spec::ImageReference& reference,
      const string& directory);

  Future<Nothing> downloadLayer(
      const spec::ImageReference& reference,
      const string& directory,
      const string& blobSum,
      const string& layerId);

  Future<vector<string>> untarLayers(
      const string& directory,
      const vector<string>& layerIds);

  Future<Nothing> untarLayer(
      const string& directory,
      const string& layerId);

  Owned<RegistryClient> registryClient_;
  const Duration pullTimeout_;
  hashmap<string, Owned<Promise<Nothing>>> downloadTracker_;

  RegistryPullerProcess(const RegistryPullerProcess&) = delete;
  RegistryPullerProcess& operator=(const RegistryPullerProcess&) = delete;
};


Try<Owned<Puller>> RegistryPuller::create(const Flags& flags)
{
  Try<Owned<RegistryPullerProcess>> process =
    RegistryPullerProcess::create(flags);

  if (process.isError()) {
    return Error(process.error());
  }

  return Owned<Puller>(new RegistryPuller(process.get()));
}


RegistryPuller::RegistryPuller(Owned<RegistryPullerProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


RegistryPuller::~RegistryPuller()
{
  terminate(process.get());
  process::wait(process.get());
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


Try<Owned<RegistryPullerProcess>> RegistryPullerProcess::create(
    const Flags& flags)
{
  Result<double> timeoutSecs = numify<double>(flags.docker_puller_timeout_secs);
  if ((timeoutSecs.isError()) || (timeoutSecs.get() <= 0)) {
    return Error(
        "Failed to create registry puller - invalid timeout value: " +
        flags.docker_puller_timeout_secs);
  }

  Try<http::URL> registryUrl = http::URL::parse(flags.docker_registry);
  if (registryUrl.isError()) {
    return Error("Failed to parse Docker registry: " + registryUrl.error());
  }

  Try<http::URL> authServerUrl = http::URL::parse(flags.docker_auth_server);
  if (authServerUrl.isError()) {
    return Error("Failed to parse Docker auth server: " +
                 authServerUrl.error());
  }

  Try<Owned<RegistryClient>> registry = RegistryClient::create(
      registryUrl.get(), authServerUrl.get());

  if (registry.isError()) {
    return Error("Failed to create registry client: " + registry.error());
  }

  return Owned<RegistryPullerProcess>(new RegistryPullerProcess(
      registry.get(),
      Seconds(timeoutSecs.get())));
}


RegistryPullerProcess::RegistryPullerProcess(
    const Owned<RegistryClient>& registry,
    const Duration& timeout)
  : registryClient_(registry),
    pullTimeout_(timeout) {}


Future<vector<string>> RegistryPullerProcess::pull(
    const spec::ImageReference& reference,
    const string& directory)
{
  // TODO(jojy): Have one outgoing manifest request per image.
  return registryClient_->getManifest(reference)
    .then(process::defer(self(), [=](const spec::v2::ImageManifest& manifest) {
      return downloadLayers(manifest, reference, directory);
    }))
    .then(process::defer(self(), [=](const vector<string>& layerIds) {
      return untarLayers(directory, layerIds);
    }))
    .after(pullTimeout_, [reference](Future<vector<string>> future) {
      future.discard();
      return Failure("Timed out");
    });
}


Future<vector<string>> RegistryPullerProcess::downloadLayers(
    const spec::v2::ImageManifest& manifest,
    const spec::ImageReference& reference,
    const string& directory)
{
  list<Future<Nothing>> futures;
  vector<string> layerIds;

  CHECK_EQ(manifest.fslayers_size(), manifest.history_size());

  for (int i = 0; i < manifest.fslayers_size(); i++) {
    CHECK(manifest.history(i).has_v1());

    layerIds.push_back(manifest.history(i).v1().id());

    futures.push_back(downloadLayer(
        reference,
        directory,
        manifest.fslayers(i).blobsum(),
        manifest.history(i).v1().id()));
  }

  // TODO(jojy): Delete downloaded files in the directory on discard and
  // failure?
  // TODO(jojy): Iterate through the futures and log the failed future.
  return collect(futures)
    .then([layerIds]() { return layerIds; });
}


Future<Nothing> RegistryPullerProcess::downloadLayer(
    const spec::ImageReference& reference,
    const string& directory,
    const string& blobSum,
    const string& layerId)
{
  VLOG(1) << "Downloading layer '"  << layerId
          << "' for image '" << stringify(reference) << "'";

  if (downloadTracker_.contains(layerId)) {
    VLOG(1) << "Download already in progress for image '"
            << stringify(reference) << "', layer '" << layerId << "'";

    return downloadTracker_.at(layerId)->future();
  }

  Owned<Promise<Nothing>> downloadPromise(new Promise<Nothing>());

  downloadTracker_.insert({layerId, downloadPromise});

  const Path downloadFile(path::join(directory, layerId + ".tar"));

  registryClient_->getBlob(
      reference,
      blobSum,
      downloadFile)
    .onAny(process::defer(
        self(),
        [this, layerId, downloadPromise, downloadFile](
            const Future<size_t>& future) {
          downloadTracker_.erase(layerId);

          if (!future.isReady()) {
              downloadPromise->fail(
                  "Failed to download layer '" + layerId + "': " +
                  (future.isFailed() ? future.failure() : "future discarded"));
          } else if (future.get() == 0) {
            // We don't expect Docker registry to return empty response
            // even with empty layers.
            downloadPromise->fail(
                "Failed to download layer '" + layerId + "': no content");
          } else {
            downloadPromise->set(Nothing());
          }
        }));

  return downloadPromise->future();
}


Future<vector<string>> RegistryPullerProcess::untarLayers(
    const string& directory,
    const vector<string>& layerIds)
{
  list<Future<Nothing>> futures;
  foreach (const string& layerId, layerIds) {
    VLOG(1) << "Untarring layer '" << layerId
            << "' downloaded from registry to directory '"
            << directory << "'";

    futures.emplace_back(untarLayer(directory, layerId));
  }

  return collect(futures)
    .then([layerIds]() { return layerIds; });
}


Future<Nothing> RegistryPullerProcess::untarLayer(
    const string& directory,
    const string& layerId)
{
  const string layerPath = path::join(directory, layerId);
  const string tar = path::join(directory, layerId + ".tar");
  const string rootfs = paths::getImageLayerRootfsPath(layerPath);

  Try<Nothing> mkdir = os::mkdir(rootfs);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create directory '" + rootfs + "'"
        ": " + mkdir.error());
  }

  return command::untar(Path(tar), Path(rootfs))
    .then([tar]() -> Future<Nothing> {
      // Remove the tar after the extraction.
      Try<Nothing> rm = os::rm(tar);
      if (rm.isError()) {
        return Failure(
          "Failed to remove '" + tar + "' "
          "after extraction: " + rm.error());
      }

      return Nothing();
    });
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
