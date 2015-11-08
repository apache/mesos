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

  Future<list<pair<string, string>>> downloadLayers(
      const Path& downloadDirectory,
      const Image::Name& imageName,
      const Manifest& manifest);

  Future<pair<string, string>> downloadLayer(
      const string& path,
      const Path& downloadDirectory,
      const FileSystemLayerInfo& layer);

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
      downloadDir);}


Try<Owned<RegistryPullerProcess>> RegistryPullerProcess::create(
    const Flags& flags)
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
    const string& path,
    const Path& downloadDirectory,
    const FileSystemLayerInfo& layer)
{
  VLOG(1) << "Downloading layer '"  << layer.layerId
          << "' for image '" << path << "'";

  if (downloadTracker_.contains(layer.layerId)) {
    VLOG(1) << "Download already in progress for image '" << path
            << "''s layer '" << layer.layerId << "'";

    return downloadTracker_.at(layer.layerId)->future();
  }

  Owned<Promise<pair<string, string>>> downloadPromise(
      new Promise<pair<string, string>>());

  downloadTracker_[layer.layerId] = downloadPromise;

  const Path downloadFile(path::join(downloadDirectory, layer.layerId));

  registryClient_->getBlob(
      path,
      layer.checksumInfo,
      downloadFile)
    .onAny(defer(
        self(),
        [this, layer, downloadPromise, downloadFile](
            const Future<size_t>& future) {
          downloadTracker_.erase(layer.layerId);

          if (!future.isReady()) {
            downloadPromise->fail(
                "Failed to download layer '" + layer.layerId +
                "': " +
                (future.isFailed() ? future.failure() : "future discarded"));
          } else if (future.get() <= 0) {
            // We don't ever expect Docker registry to return empty response
            // even for empty layers in the image.
            downloadPromise->fail(
                "Failed to download layer '" + layer.layerId + "': no content");
          } else {
            downloadPromise->set({layer.layerId, downloadFile});
          }
        }));

  return downloadPromise->future();
}


// Canonical names could be something like "ubuntu14.04" but the
// remote name for it could be library/ubuntu14.04.
// TODO(jojy): This mapping has to com from a configuration in the
// near future. Docker has different namings depending on whether its an
// "official" repo or not.
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
Try<Image::Name> RegistryPullerProcess::getRemoteNameFromLocalName(
    const Image::Name& imageName)
{
  // TODO(jojy): Support unoffical repos.
  if (strings::contains(imageName.repository(), "/")) {
    return Error("Currently only support offical repos");
  }

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


Future<list<pair<string, string>>> RegistryPullerProcess::downloadLayers(
    const Path& downloadDirectory,
    const Image::Name& remoteName,
    const Manifest& manifest)
{
  list<Future<pair<string, string>>> downloadFutures;

  foreach (const FileSystemLayerInfo& layer, manifest.fsLayerInfos) {
    downloadFutures.push_back(
        downloadLayer(
            remoteName.repository(),
            downloadDirectory,
            layer));
  }

  return collect(downloadFutures);
}


Future<list<pair<string, string>>> untarLayers(
    const Path& downloadDirectory,
    const list<pair<string, string>>& imageInfo)
{
  list<Future<Nothing>> untarFutures;

  pair<string, string> layerInfo;
  foreach (layerInfo, imageInfo) {
    untarFutures.emplace_back(untar(layerInfo.second, downloadDirectory));
  }

  return collect(untarFutures)
    .then([imageInfo]() { return imageInfo; });
}


Future<list<pair<string, string>>> RegistryPullerProcess::pull(
    const Image::Name& imageName,
    const Path& downloadDirectory)
{
  // We assume the name passed from the framework is local name.
  // For more information please refer to comments in
  // getRemoteNameFromLocalName.
  Try<Image::Name> remoteImageName = getRemoteNameFromLocalName(imageName);
  if (remoteImageName.isError()) {
    return Failure(
        "Failed to get remote name from local name for '" +
        imageName.repository() + "': " + remoteImageName.error());
  }

  // TODO(jojy): Delete downloaded files in the directory on discard and
  // failure?
  return registryClient_->getManifest(remoteImageName.get())
    .then(defer(self(),
                &Self::downloadLayers,
                downloadDirectory,
                remoteImageName.get(),
                lambda::_1))
    .then(lambda::bind(&untarLayers, downloadDirectory, lambda::_1))
    .then([](const list<pair<string, string>>& imageInfo) { return imageInfo; })
    .after(pullTimeout_, [remoteImageName](
        Future<list<pair<string, string>>> future)
           -> Future<list<pair<string, string>>> {
      future.discard();

      return Failure(
          "Failed to download image '" + remoteImageName.get().repository() +
          "': timeout");
    });
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
