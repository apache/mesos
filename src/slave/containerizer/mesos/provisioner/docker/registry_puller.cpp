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

#include "slave/containerizer/mesos/provisioner/docker/registry_puller.hpp"

#include <list>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/subprocess.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"
#include "slave/containerizer/mesos/provisioner/docker/registry_client.hpp"

namespace http = process::http;
namespace spec = docker::spec;

using std::list;
using std::pair;
using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::Promise;
using process::Subprocess;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

using RegistryClient = registry::RegistryClient;


class RegistryPullerProcess : public Process<RegistryPullerProcess>
{
public:
  static Try<Owned<RegistryPullerProcess>> create(const Flags& flags);

  process::Future<list<pair<string, string>>> pull(
      const Image::Name& imageName,
      const Path& directory);

private:
  explicit RegistryPullerProcess(
      const Owned<RegistryClient>& registry,
      const Duration& timeout);

  Future<pair<string, string>> downloadLayer(
      const Image::Name& imageName,
      const Path& directory,
      const string& blobSum,
      const string& id);

  Future<list<pair<string, string>>> downloadLayers(
      const spec::v2::ImageManifest& manifest,
      const Image::Name& imageName,
      const Path& downloadDir);

  process::Future<list<pair<string, string>>> _pull(
      const Image::Name& imageName,
      const Path& downloadDir);

  Future<list<pair<string, string>>> untarLayers(
    const Future<list<pair<string, string>>>& layerFutures,
    const Path& downloadDir);

  Owned<RegistryClient> registryClient_;
  const Duration pullTimeout_;
  hashmap<string, Owned<Promise<pair<string, string>>>> downloadTracker_;

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


RegistryPuller::RegistryPuller(const Owned<RegistryPullerProcess>& process)
  : process_(process)
{
  spawn(CHECK_NOTNULL(process_.get()));
}


RegistryPuller::~RegistryPuller()
{
  terminate(process_.get());
  process::wait(process_.get());
}


Future<list<pair<string, string>>> RegistryPuller::pull(
    const Image::Name& imageName,
    const Path& downloadDir)
{
  return dispatch(
      process_.get(),
      &RegistryPullerProcess::pull,
      imageName,
      downloadDir);
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


Future<pair<string, string>> RegistryPullerProcess::downloadLayer(
    const Image::Name& imageName,
    const Path& directory,
    const string& blobSum,
    const string& layerId)
{
  VLOG(1) << "Downloading layer '"  << layerId
          << "' for image '" << stringify(imageName) << "'";

  if (downloadTracker_.contains(layerId)) {
    VLOG(1) << "Download already in progress for image '"
            << stringify(imageName) << "', layer '" << layerId << "'";

    return downloadTracker_.at(layerId)->future();
  }

  Owned<Promise<pair<string, string>>> downloadPromise(
      new Promise<pair<string, string>>());

  downloadTracker_.insert({layerId, downloadPromise});

  const Path downloadFile(path::join(directory, layerId + ".tar"));

  registryClient_->getBlob(
      imageName,
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
            downloadPromise->set({layerId, downloadFile});
          }
        }));

  return downloadPromise->future();
}


Future<list<pair<string, string>>> RegistryPullerProcess::pull(
    const Image::Name& imageName,
    const Path& directory)
{
  // TODO(jojy): Have one outgoing manifest request per image.
  return registryClient_->getManifest(imageName)
    .then(process::defer(self(), [this, directory, imageName](
        const spec::v2::ImageManifest& manifest) {
      return downloadLayers(manifest, imageName, directory);
    }))
    .then(process::defer(self(), [this, directory](
        const Future<list<pair<string, string>>>& layerFutures)
        -> Future<list<pair<string, string>>> {
      return untarLayers(layerFutures, directory);
    }))
    .after(pullTimeout_, [imageName](
        Future<list<pair<string, string>>> future) {
      future.discard();

      return Failure("Timed out");
    });
}


Future<list<pair<string, string>>> RegistryPullerProcess::downloadLayers(
    const spec::v2::ImageManifest& manifest,
    const Image::Name& imageName,
    const Path& directory)
{
  list<Future<pair<string, string>>> downloadFutures;

  for (int i = 0; i < manifest.fslayers_size(); i++) {
    downloadFutures.push_back(
        downloadLayer(imageName,
                      directory,
                      manifest.fslayers(i).blobsum(),
                      manifest.history(i).v1compatibility().id()));
  }

  // TODO(jojy): Delete downloaded files in the directory on discard and
  // failure?
  // TODO(jojy): Iterate through the futures and log the failed future.
  return collect(downloadFutures);
}


Future<list<pair<string, string>>> RegistryPullerProcess::untarLayers(
    const Future<list<pair<string, string>>>& layerFutures,
    const Path& directory)
{
  list<Future<pair<string, string>>> untarFutures;

  pair<string, string> layerInfo;
  foreach (layerInfo, layerFutures.get()) {
    VLOG(1) << "Untarring layer '" << layerInfo.first
            << "' downloaded from registry to directory '" << directory << "'";
    untarFutures.emplace_back(
        untarLayer(layerInfo.second, directory, layerInfo.first));
  }

  return collect(untarFutures);
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
