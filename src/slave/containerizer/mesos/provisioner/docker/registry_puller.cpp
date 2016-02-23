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

  Future<Nothing> fetchLayer(
      const spec::ImageReference& reference,
      const string& directory,
      const string& blobSum,
      const spec::v1::ImageManifest& v1);

  Future<Nothing> _fetchLayer(
      const string& directory,
      const string& blobSum,
      const spec::v1::ImageManifest& v1);

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
  : storeDir(_storeDir),
    defaultRegistryUrl(_defaultRegistryUrl),
    fetcher(_fetcher) {}


Future<vector<string>> RegistryPullerProcess::pull(
    const spec::ImageReference& reference,
    const string& directory)
{
  // TODO(jieyu): Consider introducing a 'normalize' function to
  // normalize 'reference' here. For instance, we need to add
  // 'library/' prefix if the user does not specify a repository.
  // Also consider merging the registry generation logic below into
  // 'normalize'.

  URI manifestUri;
  if (reference.has_registry()) {
    // TODO(jieyu): The user specified registry might contain port. We
    // need to parse it and set the 'scheme' and 'port' accordingly.
    manifestUri = uri::docker::manifest(
        reference.repository(),
        (reference.has_tag() ? reference.tag() : "latest"),
        reference.registry());
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

  list<Future<Nothing>> futures;
  vector<string> layerIds;

  CHECK_EQ(manifest->fslayers_size(), manifest->history_size());

  for (int i = 0; i < manifest->fslayers_size(); i++) {
    CHECK(manifest->history(i).has_v1());

    layerIds.push_back(manifest->history(i).v1().id());

    futures.push_back(fetchLayer(
        reference,
        directory,
        manifest->fslayers(i).blobsum(),
        manifest->history(i).v1()));
  }

  return collect(futures)
    .then([layerIds]() { return layerIds; });
}


Future<Nothing> RegistryPullerProcess::fetchLayer(
    const spec::ImageReference& reference,
    const string& directory,
    const string& blobSum,
    const spec::v1::ImageManifest& v1)
{
  // Check if the layer is in the store or not. If yes, skip the
  // unnecessary fetching.
  if (os::exists(paths::getImageLayerPath(storeDir, v1.id()))) {
    return Nothing();
  }

  VLOG(1) << "Fetching layer '" << v1.id() << "' for image '"
          << reference << "'";

  URI blobUri;
  if (reference.has_registry()) {
    // TODO(jieyu): The user specified registry might contain port. We
    // need to parse it and set the 'scheme' and 'port' accordingly.
    blobUri = uri::docker::blob(
        reference.repository(),
        blobSum,
        reference.registry());
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

  return fetcher->fetch(blobUri, directory)
    .then(defer(self(), &Self::_fetchLayer, directory, blobSum, v1));
}


Future<Nothing> RegistryPullerProcess::_fetchLayer(
    const string& directory,
    const string& blobSum,
    const spec::v1::ImageManifest& v1)
{
  const string layerPath = path::join(directory, v1.id());
  const string tar = path::join(directory, blobSum);
  const string rootfs = paths::getImageLayerRootfsPath(layerPath);
  const string manifest = paths::getImageLayerManifestPath(layerPath);

  // NOTE: This will create 'layerPath' as well.
  Try<Nothing> mkdir = os::mkdir(rootfs, true);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create rootfs directory '" + rootfs + "' "
        "for layer '" + v1.id() + "': " + mkdir.error());
  }

  Try<Nothing> write = os::write(manifest, stringify(JSON::protobuf(v1)));
  if (write.isError()) {
    return Failure(
        "Failed to save the layer manifest for layer '" +
        v1.id() + "': " + write.error());
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
