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

#include "slave/containerizer/mesos/provisioner/docker/remote_puller.hpp"

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

using FileSystemLayerInfo = registry::FileSystemLayerInfo;
using Manifest = registry::Manifest;
using RegistryClient = registry::RegistryClient;


class RemotePullerProcess : public Process<RemotePullerProcess>
{
public:
  static Try<Owned<RemotePullerProcess>> create(const Flags& flags);

  process::Future<PulledImageInfo> pull(
      const Image::Name& imageName,
      const Path& downloadDir);

  Duration getTimeout() const
  {
    return pullTimeout_;
  }

private:
  explicit RemotePullerProcess(
      const Owned<RegistryClient>& registry,
      const Duration& timeout);

  Try<Image::Name> getRemoteNameFromLocalName(const Image::Name& name);

  Future<PulledLayerInfo> downloadLayer(
      const string& remoteName,
      const Path& downloadDir,
      const FileSystemLayerInfo& layer);

  Owned<RegistryClient> registryClient_;
  const Duration pullTimeout_;
  hashmap<string, Owned<Promise<PulledLayerInfo>>> downloadTracker_;

  RemotePullerProcess(const RemotePullerProcess&) = delete;
  RemotePullerProcess& operator=(const RemotePullerProcess&) = delete;
};


Try<Owned<Puller>> RemotePuller::create(const Flags& flags)
{
  Try<Owned<RemotePullerProcess>> process = RemotePullerProcess::create(flags);

  if (process.isError()) {
    return Error(process.error());
  }

  return Owned<Puller>(new RemotePuller(process.get()));
}


RemotePuller::RemotePuller(const Owned<RemotePullerProcess>& process)
  : process_(process)
{
  spawn(CHECK_NOTNULL(process_.get()));
}


RemotePuller::~RemotePuller()
{
  terminate(process_.get());
  process::wait(process_.get());
}


Future<PulledImageInfo> RemotePuller::pull(
    const Image::Name& imageName,
    const Path& downloadDir)
{
  return dispatch(
      process_.get(),
      &RemotePullerProcess::pull,
      imageName,
      downloadDir)
    // We have 'after' here because the local puller need not have a timeout.
    // Timeout is a flag for the remote puller and needs to be handled here.
    .after(process_->getTimeout(), [imageName](
        Future<PulledImageInfo> future) {
      LOG(WARNING) << "Failed to download image '"
                   << imageName.repository()
                   << "': timeout";
      future.discard();

      return Failure(
          "Failed to download image '" + imageName.repository() + "': timeout");
    });
}


Try<Owned<RemotePullerProcess>> RemotePullerProcess::create(const Flags& flags)
{
  Result<double> timeoutSecs = numify<double>(flags.docker_puller_timeout_secs);
  if ((timeoutSecs.isError()) || (timeoutSecs.get() <= 0)) {
    return Error(
        "Failed to create remote puller - invalid timeout value: " +
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

  return Owned<RemotePullerProcess>(new RemotePullerProcess(
      registry.get(),
      Seconds(timeoutSecs.get())));
}


RemotePullerProcess::RemotePullerProcess(
    const Owned<RegistryClient>& registry,
    const Duration& timeout)
  : registryClient_(registry),
    pullTimeout_(timeout) {}


Future<PulledLayerInfo> RemotePullerProcess::downloadLayer(
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
    .onAny(defer(
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


Try<Image::Name> RemotePullerProcess::getRemoteNameFromLocalName(
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


Future<PulledImageInfo> RemotePullerProcess::pull(
    const Image::Name& imageName,
    const Path& downloadDir)
{
  Try<Image::Name> remoteImageName = getRemoteNameFromLocalName(imageName);
  if (remoteImageName.isError()) {
    return Failure(
        "Failed to get remote name from local name for '" +
        imageName.repository() + "': " + remoteImageName.error());
  }


  // TODO(jojy): Delete downloaded files in the directory on discard and
  // failure?
  // TODO(jojy): Iterate through the futures and log the failed future.
  return registryClient_->getManifest(remoteImageName.get())
    .then(defer(self(), [this, downloadDir, remoteImageName](
        const Manifest& manifest) {
      list<Future<PulledLayerInfo>> downloadFutures;

      foreach (const FileSystemLayerInfo& layer, manifest.fsLayerInfos) {
        downloadFutures.push_back(downloadLayer(
            remoteImageName.get().repository(),
            downloadDir,
            layer));
      }

      return collect(downloadFutures);
    }))
    .then([downloadDir]
        (const Future<list<PulledLayerInfo>>& layerFutures)
        -> Future<PulledImageInfo> {
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
    });
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
