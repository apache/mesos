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

#include "slave/containerizer/mesos/provisioner/docker/registry_puller.hpp"

#include <list>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/subprocess.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/mesos/provisioner/docker/registry_client.hpp"

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

namespace http = process::http;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

using FileSystemLayerInfo = registry::FileSystemLayerInfo;
using Manifest = registry::Manifest;
using RegistryClient = registry::RegistryClient;


class RegistryPullerProcess : public Process<RegistryPullerProcess>
{
public:
  static Try<Owned<RegistryPullerProcess>> create(const Flags& flags);

  process::Future<list<pair<string, string>>> pull(
      const Image::Name& imageName,
      const Path& downloadDirectory);

private:
  explicit RegistryPullerProcess(
      const Owned<RegistryClient>& registry,
      const Duration& timeout);

  Future<pair<string, string>> downloadLayer(
      const Image::Name& imageName,
      const Path& downloadDirectory,
      const FileSystemLayerInfo& layer);

  Future<list<pair<string, string>>> downloadLayers(
      const Manifest& manifest,
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

  auto getPort = [](
      const string& stringVal) -> Try<uint16_t> {
    Result<uint16_t> port = numify<uint16_t>(stringVal);
    if (port.isError()) {
      return Error("Failed to parse '" + stringVal + "' as port");
    }

    return port.get();
  };

  // TODO(jojy): Maybe do a nslookup/tcp dial of these servers.
  Try<uint16_t> registryPort = getPort(flags.docker_registry_port);
  if (registryPort.isError()) {
    return Error("Failed to parse registry port: " + registryPort.error());
  }

  Try<uint16_t> authServerPort = getPort(flags.docker_auth_server_port);
  if (authServerPort.isError()) {
    return Error(
        "Failed to parse authentication server port: " +
        authServerPort.error());
  }

  Try<Owned<RegistryClient>> registry = RegistryClient::create(
      http::URL("https", flags.docker_registry, registryPort.get()),
      http::URL("https", flags.docker_auth_server, authServerPort.get()),
      None());

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
    const Path& downloadDirectory,
    const FileSystemLayerInfo& layer)
{
  VLOG(1) << "Downloading layer '"  << layer.layerId
          << "' for image '" << stringify(imageName) << "'";

  if (downloadTracker_.contains(layer.layerId)) {
    VLOG(1) << "Download already in progress for image '"
            << stringify(imageName) << "', layer '" << layer.layerId << "'";

    return downloadTracker_.at(layer.layerId)->future();
  }

  Owned<Promise<pair<string, string>>> downloadPromise(
      new Promise<pair<string, string>>());

  downloadTracker_.insert({layer.layerId, downloadPromise});

  const Path downloadFile(path::join(downloadDirectory, layer.layerId));

  registryClient_->getBlob(
      imageName,
      layer.checksumInfo,
      downloadFile)
    .onAny(process::defer(
        self(),
        [this, layer, downloadPromise, downloadFile](
            const Future<size_t>& future) {
          downloadTracker_.erase(layer.layerId);

          if (!future.isReady()) {
              downloadPromise->fail(
                  "Failed to download layer '" + layer.layerId + "': " +
                  (future.isFailed() ? future.failure() : "future discarded"));
          } else if (future.get() == 0) {
            // We don't expect Docker registry to return empty response
            // even with empty layers.
            downloadPromise->fail(
                "Failed to download layer '" + layer.layerId + "': no content");
          } else {
            downloadPromise->set({layer.layerId, downloadFile});
          }
        }));

  return downloadPromise->future();
}


Future<Nothing> untar(const string& file, const string& directory)
{
  const vector<string> argv = {
    "tar",
    "-C",
    directory,
    "-x",
    "-f",
    file
  };

  Try<Subprocess> s = subprocess(
      "tar",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"));

  if (s.isError()) {
    return Failure(
        "Failed to create untar subprocess for file '" +
        file + "': " + s.error());
  }

  return s.get().status()
    .then([file](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure(
            "Failed to reap untar subprocess for file '" + file + "'");
      }

      if (!WIFEXITED(status.get()) ||
          WEXITSTATUS(status.get()) != 0) {
        return Failure(
            "Untar process for file '" + file + "' failed with exit code: " +
            WSTRINGIFY(status.get()));
      }

      return Nothing();
    });
}


Future<list<pair<string, string>>> RegistryPullerProcess::pull(
    const Image::Name& imageName,
    const Path& downloadDirectory)
{
  // TODO(jojy): Have one outgoing manifest request per image.
  return registryClient_->getManifest(imageName)
    .then(process::defer(self(), [this, downloadDirectory, imageName](
        const Manifest& manifest) {
      return downloadLayers(manifest, imageName, downloadDirectory);
    }))
    .then(process::defer(self(), [this, downloadDirectory](
        const Future<list<pair<string, string>>>& layerFutures)
        -> Future<list<pair<string, string>>> {
      return untarLayers(layerFutures, downloadDirectory);
    }))
    .after(pullTimeout_, [imageName](
        Future<list<pair<string, string>>> future) {
      future.discard();

      return Failure("Timed out");
    });
}


Future<list<pair<string, string>>> RegistryPullerProcess::downloadLayers(
    const Manifest& manifest,
    const Image::Name& imageName,
    const Path& downloadDirectory)
{
  list<Future<pair<string, string>>> downloadFutures;

  foreach (const FileSystemLayerInfo& layer, manifest.fsLayerInfos) {
    downloadFutures.push_back(
        downloadLayer(imageName, downloadDirectory, layer));
  }

  // TODO(jojy): Delete downloaded files in the directory on discard and
  // failure?
  // TODO(jojy): Iterate through the futures and log the failed future.
  return collect(downloadFutures);
}


Future<list<pair<string, string>>> RegistryPullerProcess::untarLayers(
    const Future<list<pair<string, string>>>& layerFutures,
    const Path& downloadDirectory)
{
  list<Future<Nothing>> untarFutures;

  pair<string, string> layerInfo;
  foreach (layerInfo, layerFutures.get()) {
    untarFutures.emplace_back(untar(layerInfo.second, downloadDirectory));
  }

  return collect(untarFutures)
    .then([layerFutures]() {
      list<pair<string, string>> layers;

      pair<string, string> layerInfo;
      foreach (layerInfo, layerFutures.get()) {
        layers.emplace_back(layerInfo);
      }

      return layers;
    });
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
