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

#include <stout/os.hpp>
#include <stout/strings.hpp>

#include <mesos/mesos.hpp>

#include "common/command_utils.hpp"

#include "slave/containerizer/mesos/provisioner/appc/fetcher.hpp"
#include "slave/containerizer/mesos/provisioner/appc/paths.hpp"

#include "uri/schemes/file.hpp"
#include "uri/schemes/http.hpp"

namespace http = process::http;

using std::string;

using process::Failure;
using process::Future;
using process::Owned;
using process::Shared;

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

static const char EXT[] = "aci";
static const char LABEL_VERSION[] = "version";
static const char LABEL_OS[] = "os";
static const char LABEL_ARCH[] = "arch";


static Try<string> getSimpleDiscoveryImagePath(const Image::Appc& appc)
{
  CHECK(!appc.name().empty());

  hashmap<string, string> labels;
  foreach (const mesos::Label& label, appc.labels().labels()) {
    labels[label.key()] = label.value();
  }

  if (!labels.contains(LABEL_VERSION)) {
    labels.insert({LABEL_VERSION, "latest"});
  }

  if (!labels.contains(LABEL_OS)) {
    return Error(
        "Failed to form simple discovery url: label '" +
        string(LABEL_OS) + "' is missing");
  }

  if (!labels.contains(LABEL_ARCH)) {
    return Error(
        "Failed to form simple discovery url: label '" +
        string(LABEL_ARCH) + "' is missing");
  }

  return strings::format(
      "%s-%s-%s-%s.%s",
      appc.name(),              // Image name.
      labels.at(LABEL_VERSION), // Version label.
      labels.at(LABEL_OS),      // OS label.
      labels.at(LABEL_ARCH),    // ARCH label.
      EXT);                     // Extension.
}


static Try<URI> getUri(const string& prefix, const string& path)
{
  const string rawUrl = prefix + path;

  // TODO(jojy): Add parse URI function in URI namespace.

  // TODO(jojy): Add `Path::seperator` which abstracts the file
  // separator character.
  if (strings::startsWith(rawUrl, "/")) {
    return uri::file(rawUrl);
  }

  Try<http::URL> _url = http::URL::parse(rawUrl);
  if (_url.isError()) {
    return Error(
        "Failed to parse '" + rawUrl + "' as a valid URL: " + _url.error());
  }

  const http::URL& url = _url.get();

  if (url.domain.isNone() && url.ip.isNone()) {
    return Error(
        "Failed to parse host name from image url '" + rawUrl + "'");
  }

  // Get image server port.
  if (url.port.isNone()) {
    return Error(
        "Failed to parse port for image url '" + rawUrl + "'");
  }

  // Get image server host.
  const string host = url.domain.isSome()
    ? url.domain.get()
    : stringify(url.ip.get());

  int port = static_cast<int>(url.port.get());

  if (url.scheme.get() == "http") {
    return uri::http(host, url.path, port);
  }

  if (url.scheme.get() == "https") {
    return uri::https(host, url.path, port);
  }

  // TODO(jojy): Add support for hdfs.

  return Error("Unsupported scheme '" + url.scheme.get() + "'");
}


Try<Owned<Fetcher>> Fetcher::create(
    const Flags& flags,
    const Shared<uri::Fetcher>& fetcher)
{
  const string prefix = flags.appc_simple_discovery_uri_prefix;

  // TODO(jojy): Add support for hdfs.
  if (!strings::startsWith(prefix, "http") &&
      !strings::startsWith(prefix, "https") &&
      !strings::startsWith(prefix, "/")) {
    return Error("Invalid simple discovery uri prefix: " + prefix);
  }

  return new Fetcher(prefix, fetcher);
}


Fetcher::Fetcher(
    const string& _uriPrefix,
    const Shared<uri::Fetcher>& _fetcher)
  : uriPrefix(_uriPrefix),
    fetcher(_fetcher) {}


Future<Nothing> Fetcher::fetch(const Image::Appc& appc, const Path& directory)
{
  // TODO(jojy): Add more discovery implementations. We use simple
  // discovery now but in the future, this will be extended to use
  // multiple discoveries and try sequentially based on priority.

  if (appc.name().empty()) {
    return Failure("Image name cannot be empty");
  }

  Try<string> path = getSimpleDiscoveryImagePath(appc);
  if (path.isError()) {
    return Failure(
        "Failed to get discovery path for image '" +
        appc.name() + "': " + path.error());
  }

  // First construct a URI based on the scheme.
  Try<URI> uri = getUri(uriPrefix, path.get());
  if (uri.isError()) {
    return Failure(
        "Failed to get URI for image discovery path '" +
        path.get() + "': " + uri.error());
  }

  VLOG(1) << "Fetching image from URI '" << uri.get() << "'";

  // NOTE: URI fetcher will fetch the image into 'directory' with file
  // name as URI's basename.
  const Path aciBundle(path::join(
      directory,
      Path(uri->path()).basename()));

  return fetcher->fetch(uri.get(), directory)
    .then([=]() -> Future<Nothing> {
      // Change the extension to ".gz" as gzip utility expects it.
      const Path _aciBundle(aciBundle.string() + ".gz");

      Try<Nothing> rename = os::rename(aciBundle, _aciBundle);
      if (rename.isError()) {
        return Failure(
            "Failed to change extension to 'gz' for bundle '" +
            stringify(aciBundle) + "': " + rename.error());
      }

      return command::decompress(_aciBundle);
    })
    .then([=]() -> Future<string> {
      return command::sha512(aciBundle);
    })
    .then([=](const string& shasum) -> Future<Nothing> {
      const string imagePath(path::join(directory, "sha512-" + shasum));

      Try<Nothing> mkdir = os::mkdir(imagePath);
      if (mkdir.isError()) {
        return Failure(
            "Failed to create directory for untarring image '" +
            appc.name() + "': " + mkdir.error());
      }

      return command::untar(aciBundle, imagePath);
    })
    .then([=]() -> Future<Nothing> {
      // Remove the bundle file if everything goes well.
      Try<Nothing> remove = os::rm(aciBundle);
      if (remove.isError()) {
        return Failure(
            "Failed to remove aci bundle file '" + stringify(aciBundle) +
            "': " + remove.error());
      }

      return Nothing();
    });
}

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
