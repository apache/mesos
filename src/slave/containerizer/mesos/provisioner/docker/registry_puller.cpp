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


using PulledLayerInfo = RegistryPuller::PulledLayerInfo;
using PulledImageInfo = RegistryPuller::PulledImageInfo;

using FileSystemLayerInfo = registry::FileSystemLayerInfo;
using Manifest = registry::Manifest;
using RegistryClient = registry::RegistryClient;


class RegistryPullerProcess : public Process<RegistryPullerProcess>
{
public:
  static Try<Owned<RegistryPullerProcess>> create(const Flags& flags);

  process::Future<PulledImageInfo> pull(
      const Image::Name& imageName,
      const Path& downloadDir);

  Duration getTimeout() const
  {
    return pullTimeout_;
  }

private:
  explicit RegistryPullerProcess(
      const Owned<RegistryClient>& registry,
      const Duration& timeout);

  Try<Image::Name> getRemoteNameFromLocalName(const Image::Name& name);

  Future<PulledLayerInfo> downloadLayer(
      const string& remoteName,
      const Path& downloadDir,
      const FileSystemLayerInfo& layer);

  Future<PulledImageInfo> downloadLayers(
      const Manifest& manifest,
      const Image::Name& remoteImageName,
      const Path& downloadDir);

  process::Future<PulledImageInfo> _pull(
      const Image::Name& imageName,
      const Path& downloadDir);

  Future<PulledImageInfo> untarLayers(
    const Future<list<PulledLayerInfo>>& layerFutures,
    const Path& downloadDir);

  Owned<RegistryClient> registryClient_;
  const Duration pullTimeout_;
  hashmap<string, Owned<Promise<PulledLayerInfo>>> downloadTracker_;

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


Future<PulledImageInfo> RegistryPuller::pull(
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


Future<PulledLayerInfo> RegistryPullerProcess::downloadLayer(
    const string& remoteName,
    const Path& downloadDir,
    const FileSystemLayerInfo& layer)
{
  VLOG(1) << "Downloading layer '"  << layer.layerId
          << "' for image '" << remoteName << "'";

  Owned<Promise<PulledLayerInfo>> downloadPromise;

  if (!downloadTracker_.contains(layer.layerId)) {
    downloadPromise =
      Owned<Promise<PulledLayerInfo>>(new Promise<PulledLayerInfo>());

    downloadTracker_.insert({layer.layerId, downloadPromise});
  } else {
    VLOG(1) << "Download already in progress for image '" << remoteName
            << "''s layer '" << layer.layerId << "'";

    return downloadTracker_.at(layer.layerId)->future();
  }

  const Path downloadFile(path::join(downloadDir, layer.layerId));

  registryClient_->getBlob(
      remoteName,
      layer.checksumInfo,
      downloadFile)
    .onAny(process::defer(
        self(),
        [this, remoteName, downloadDir, layer, downloadPromise, downloadFile](
            const Future<size_t>& blobSizeFuture) {
          downloadTracker_.erase(layer.layerId);

          if(!blobSizeFuture.isReady()) {
            if (blobSizeFuture.isDiscarded()) {
              downloadPromise->fail(
                  "Failed to download layer '" + layer.layerId +
                  "': future discarded");
            } else {
              downloadPromise->fail(
                  "Failed to download layer '" + layer.layerId + "': " +
                  blobSizeFuture.failure());
            }

            return;
          }

          if (blobSizeFuture.get() <= 0) {
            downloadPromise->fail(
                "Failed to download layer '" + layer.layerId + "': no content");
            return;
          }

          downloadPromise->set(PulledLayerInfo(layer.layerId, downloadFile));
        }));

  return downloadPromise->future();
}


Try<Image::Name> RegistryPullerProcess::getRemoteNameFromLocalName(
    const Image::Name& imageName)
{
  //TODO(jojy): Canonical names could be something like "ubuntu14.04" but the
  //remote name for it could be library/ubuntu14.04. This mapping has to come
  //from a configuration in the near future. Docker has different namings
  //depending on whether its an "official" repo or not.

  // From the docker github code documentation:
  // RepositoryInfo Examples:
  // {
  //   "Index" : {
  //     "Name" : "docker.io",
  //     "Mirrors" : ["https://registry-2.docker.io/v1/",
  //                  "https://registry-3.docker.io/v1/"],
  //     "Secure" : true,
  //     "Official" : true,
  //   },
  //   "RemoteName" : "library/debian",
  //   "LocalName" : "debian",
  //   "CanonicalName" : "docker.io/debian"
  //   "Official" : true,
  // }
  //
  // {
  //   "Index" : {
  //     "Name" : "127.0.0.1:5000",
  //     "Mirrors" : [],
  //     "Secure" : false,
  //     "Official" : false,
  //   },
  //   "RemoteName" : "user/repo",
  //   "LocalName" : "127.0.0.1:5000/user/repo",
  //   "CanonicalName" : "127.0.0.1:5000/user/repo",
  //   "Official" : false,
  // }

  // For now, assuming that its an "official" repo.
  if (strings::contains(imageName.repository(), "/")) {
    return Error("Unexpected '/' in local image name");
  }

  // Create a remote Image::Name from the given input name.
  Image::Name remoteImageName = imageName;
  remoteImageName.set_repository("library/" + imageName.repository());

  return remoteImageName;
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


Future<PulledImageInfo> RegistryPullerProcess::pull(
    const Image::Name& imageName,
    const Path& downloadDir)
{
  return _pull(imageName, downloadDir)
    .after(getTimeout(), [imageName](
        Future<PulledImageInfo> future) {
      LOG(WARNING) << "Failed to download image '"
                   << imageName.repository()
                   << "': timeout";
      future.discard();

      return Failure(
          "Failed to download image '" + imageName.repository() + "': timeout");
    });
}


Future<PulledImageInfo> RegistryPullerProcess::downloadLayers(
    const Manifest& manifest,
    const Image::Name& remoteImageName,
    const Path& downloadDir)
{
  list<Future<PulledLayerInfo>> downloadFutures;

  foreach (const FileSystemLayerInfo& layer, manifest.fsLayerInfos) {
    downloadFutures.push_back(downloadLayer(
        remoteImageName.repository(),
        downloadDir,
        layer));
  }

  // TODO(jojy): Delete downloaded files in the directory on discard and
  // failure?
  // TODO(jojy): Iterate through the futures and log the failed future.
  return collect(downloadFutures);
}


Future<PulledImageInfo> RegistryPullerProcess::untarLayers(
    const Future<list<PulledLayerInfo>>& layerFutures,
    const Path& downloadDir)
{
  list<Future<Nothing>> untarFutures;

  foreach (const PulledLayerInfo layerInfo, layerFutures.get()) {
    untarFutures.emplace_back(untar(layerInfo.second, downloadDir));
  }

  return collect(untarFutures)
    .then([layerFutures]() {
      PulledImageInfo layers;

      foreach (const PulledLayerInfo layerInfo, layerFutures.get()) {
        layers.emplace_back(layerInfo);
      }

      return layers;
    });
}


Future<PulledImageInfo> RegistryPullerProcess::_pull(
    const Image::Name& imageName,
    const Path& downloadDir)
{
  Try<Image::Name> remoteImageName = getRemoteNameFromLocalName(imageName);
  if (remoteImageName.isError()) {
    return Failure(
        "Failed to get remote name from local name for '" +
        imageName.repository() + "': " + remoteImageName.error());
  }

  return registryClient_->getManifest(remoteImageName.get())
    .then(process::defer(self(), [this, downloadDir, remoteImageName](
        const Manifest& manifest) {
      return downloadLayers(manifest, remoteImageName.get(), downloadDir);
    }))
    .then(process::defer(self(), [this, downloadDir](
        const Future<list<PulledLayerInfo>>& layerFutures)
        -> Future<PulledImageInfo> {
      return untarLayers(layerFutures, downloadDir);
    }));
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
