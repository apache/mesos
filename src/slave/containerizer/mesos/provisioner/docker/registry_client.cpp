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

#include <vector>
#include <ostream>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os/close.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/open.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "slave/containerizer/mesos/provisioner/docker/registry_client.hpp"
#include "slave/containerizer/mesos/provisioner/docker/token_manager.hpp"

using std::ostringstream;
using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;

namespace http = process::http;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace registry {


static const uint16_t DEFAULT_SSL_PORT = 443;


class RegistryClientProcess : public Process<RegistryClientProcess>
{
public:
  static Try<Owned<RegistryClientProcess>> create(
      const http::URL& registry,
      const http::URL& authenticationServer,
      const Option<Credentials>& credentials);

  Future<Manifest> getManifest(
      const Image::Name& imageName);

  Future<size_t> getBlob(
      const string& path,
      const Option<string>& digest,
      const Path& filePath);

private:
  RegistryClientProcess(
      const http::URL& registryServer,
      const Owned<TokenManager>& tokenManager,
      const Option<Credentials>& credentials);

  Future<http::Response> doHttpGet(
      const http::URL& url,
      const Option<http::Headers>& headers,
      bool resend,
      const Option<string>& lastResponse) const;

  Try<http::Headers> getAuthenticationAttributes(
      const http::Response& httpResponse) const;

  Future<http::Response> handleHttpBadResponse(
      const http::Response& httpResponse);

  Future<http::Response> handleHttpUnauthResponse(
      const http::Response& httpResponse,
      const http::URL& url);

  Future<http::Response> handleHttpRedirect(
      const http::Response& httpResponse,
      const Option<http::Headers>& headers);

  const http::URL registryServer_;
  Owned<TokenManager> tokenManager_;
  const Option<Credentials> credentials_;

  RegistryClientProcess(const RegistryClientProcess&) = delete;
  RegistryClientProcess& operator = (const RegistryClientProcess&) = delete;
};


Try<Owned<RegistryClient>> RegistryClient::create(
    const http::URL& registryServer,
    const http::URL& authorizationServer,
    const Option<Credentials>& credentials)
{
  Try<Owned<RegistryClientProcess>> process =
    RegistryClientProcess::create(
        registryServer,
        authorizationServer,
        credentials);

  if (process.isError()) {
    return Error(process.error());
  }

  return Owned<RegistryClient>(
      new RegistryClient(
          registryServer,
          authorizationServer,
          credentials,
          process.get()));
}


RegistryClient::RegistryClient(
    const http::URL& registryServer,
    const http::URL& authorizationServer,
    const Option<Credentials>& credentials,
    const Owned<RegistryClientProcess>& process)
  : registryServer_(registryServer),
    authorizationServer_(authorizationServer),
    credentials_(credentials),
    process_(process)
{
  spawn(CHECK_NOTNULL(process_.get()));
}


RegistryClient::~RegistryClient()
{
  terminate(process_.get());
  process::wait(process_.get());
}


Future<Manifest> RegistryClient::getManifest(
    const Image::Name& imageName)
{
  return dispatch(
      process_.get(),
      &RegistryClientProcess::getManifest,
      imageName);
}


Future<size_t> RegistryClient::getBlob(
    const string& _path,
    const Option<string>& _digest,
    const Path& _filePath)
{
  return dispatch(
        process_.get(),
        &RegistryClientProcess::getBlob,
        _path,
        _digest,
        _filePath);
}


Try<Owned<RegistryClientProcess>> RegistryClientProcess::create(
    const http::URL& registryServer,
    const http::URL& authorizationServer,
    const Option<Credentials>& credentials)
{
  Try<Owned<TokenManager>> tokenMgr = TokenManager::create(authorizationServer);
  if (tokenMgr.isError()) {
    return Error("Failed to create token manager: " + tokenMgr.error());
  }

  return Owned<RegistryClientProcess>(
      new RegistryClientProcess(registryServer, tokenMgr.get(), credentials));
}


RegistryClientProcess::RegistryClientProcess(
    const http::URL& registryServer,
    const Owned<TokenManager>& tokenMgr,
    const Option<Credentials>& credentials)
  : registryServer_(registryServer),
    tokenManager_(tokenMgr),
    credentials_(credentials) {}


// RFC6750, section 3.
Try<http::Headers> RegistryClientProcess::getAuthenticationAttributes(
    const http::Response& httpResponse) const
{
  if (httpResponse.headers.find("WWW-Authenticate") ==
      httpResponse.headers.end()) {
    return Error("Failed to find WWW-Authenticate header value");
  }

  const string& authString = httpResponse.headers.at("WWW-Authenticate");

  const vector<string> authStringTokens = strings::tokenize(authString, " ");
  if ((authStringTokens.size() != 2) || (authStringTokens[0] != "Bearer")) {
    // TODO(jojy): Look at various possibilities of auth response. We currently
    // assume that the string will have realm information.
    return Error("Invalid authentication header value: " + authString);
  }

  const vector<string> authenticationParams =
    strings::tokenize(authStringTokens[1], ",");

  http::Headers authenticationAttributes;

  foreach (const string& param, authenticationParams) {
    const vector<string> paramTokens = strings::tokenize(param, "=\"");

    if (paramTokens.size() != 2) {
      return Error(
          "Failed to get authentication attribute from response parameter " +
          param);
    }

    authenticationAttributes.insert({paramTokens[0], paramTokens[1]});
  }

  return authenticationAttributes;
}


Future<http::Response> RegistryClientProcess::handleHttpUnauthResponse(
    const http::Response& httpResponse,
    const http::URL& url)
{
  Try<http::Headers> authenticationAttributes =
    getAuthenticationAttributes(httpResponse);

  if (authenticationAttributes.isError()) {
    return Failure(
        "Failed to get authentication attributes: " +
        authenticationAttributes.error());
  }

  if (!authenticationAttributes.get().contains("service")) {
    return Failure(
        "Failed to find authentication attribute \"service\" in response"
        "from authorization server");
  }

  if (!authenticationAttributes.get().contains("scope")) {
    return Failure(
        "Failed to find authentication attribute \"scope\" in response"
        "from authorization server");
  }

  // TODO(jojy): Currently only handling TLS/cert authentication.
  Future<Token> tokenResponse = tokenManager_->getToken(
      authenticationAttributes.get().at("service"),
      authenticationAttributes.get().at("scope"),
      None());

  return tokenResponse
    .then(defer(self(), [=](const Future<Token>& tokenResponse) {
      // Send request with acquired token.
      http::Headers authHeaders = {
        {"Authorization", "Bearer " + tokenResponse.get().raw}
      };

      return doHttpGet(
          url,
          authHeaders,
          true,
          httpResponse.status);
    }));
}


Future<http::Response> RegistryClientProcess::handleHttpBadResponse(
    const http::Response& httpResponse)
{
  Try<JSON::Object> errorResponse =
    JSON::parse<JSON::Object>(httpResponse.body);

  if (errorResponse.isError()) {
    return Failure("Failed to parse bad request response JSON: " +
        errorResponse.error());
  }

  std::ostringstream out;
  bool first = true;
  Result<JSON::Array> errorObjects =
    errorResponse.get().find<JSON::Array>("errors");

  if (errorObjects.isError()) {
    return Failure("Failed to find 'errors' in bad request response: " +
        errorObjects.error());
  } else if (errorObjects.isNone()) {
    return Failure("Errors not found in bad request response");
  }

  foreach (const JSON::Value& error, errorObjects.get().values) {
    if (!error.is<JSON::Object>()) {
      LOG(WARNING) <<
        "Failed to parse error message: "
        "'error' expected to be JSON object";

      continue;
    }

    Result<JSON::String> message =
      error.as<JSON::Object>().find<JSON::String>("message");

    if (message.isError()) {
      return Failure("Failed to parse bad request error message: " +
          message.error());
    } else if (message.isNone()) {
      continue;
    }

    if (first) {
      out << message.get().value;
      first = false;
    } else {
      out << ", " << message.get().value;
    }
  }

  return Failure("Received Bad request, errors: [" + out.str() + "]");
}


Future<http::Response> RegistryClientProcess::handleHttpRedirect(
    const http::Response& httpResponse,
    const Option<http::Headers>& headers)
{
  // TODO(jojy): Add redirect functionality in http::get.
  auto toURL = [](
      const string& urlString) -> Try<http::URL> {
    // TODO(jojy): Need to add functionality to URL class that parses a
    // string to its URL components. For now, assuming:
    //  - scheme is https
    //  - path always ends with /

    static const string schemePrefix = "https://";

    if (!strings::contains(urlString, schemePrefix)) {
      return Error(
          "Failed to find expected token '" + schemePrefix +
          "' in redirect url");
    }

    const string schemeSuffix = urlString.substr(schemePrefix.length());

    const vector<string> components =
      strings::tokenize(schemeSuffix, "/");

    const string path = schemeSuffix.substr(components[0].length());

    const vector<string> addrComponents =
      strings::tokenize(components[0], ":");

    uint16_t port = DEFAULT_SSL_PORT;
    string domain = components[0];

    // Parse the port.
    if (addrComponents.size() == 2) {
      domain = addrComponents[0];

      Try<uint16_t> tryPort = numify<uint16_t>(addrComponents[1]);
      if (tryPort.isError()) {
        return Error(
            "Failed to parse location: " + urlString + " for port.");
      }

      port = tryPort.get();
    }

    return http::URL("https", domain, port, path);
  };

  if (httpResponse.headers.find("Location") ==
      httpResponse.headers.end()) {
    return Failure(
        "Invalid redirect response: 'Location' not found in headers.");
  }

  const string& location = httpResponse.headers.at("Location");
  Try<http::URL> tryUrl = toURL(location);
  if (tryUrl.isError()) {
    return Failure(
        "Failed to parse '" + location + "': " + tryUrl.error());
  }

  return doHttpGet(
      tryUrl.get(),
      headers,
      false,
      httpResponse.status);
}


Future<http::Response> RegistryClientProcess::doHttpGet(
    const http::URL& url,
    const Option<http::Headers>& headers,
    bool resend,
    const Option<string>& lastResponseStatus) const
{
  return http::get(url, headers)
    .then(defer(self(), [=](const http::Response& httpResponse)
        -> Future<http::Response> {
      VLOG(1) << "Response status: " + httpResponse.status;

      // Set the future if we get a OK response.
      if (httpResponse.status == "200 OK") {
        return httpResponse;
      }

      if (httpResponse.status == "400 Bad Request") {
        return handleHttpBadResponse(httpResponse);
      }

      // Prevent infinite recursion.
      if (lastResponseStatus.isSome() &&
          (lastResponseStatus.get() == httpResponse.status)) {
        return Failure("Invalid response: " + httpResponse.status);
      }

      // If resend is not set, we dont try again and stop here.
      if (!resend) {
        return Failure("Bad response: " + httpResponse.status);
      }

      // Handle 401 Unauthorized.
      if (httpResponse.status == "401 Unauthorized") {
        return handleHttpUnauthResponse(httpResponse, url);
      }

      // Handle redirect.
      if (httpResponse.status == "307 Temporary Redirect") {
        return handleHttpRedirect(httpResponse, headers);
      }

      return Failure("Invalid response: " + httpResponse.status);
    }));
}


Try<Manifest> Manifest::create(const string& jsonString)
{
    Try<JSON::Object> manifestJSON = JSON::parse<JSON::Object>(jsonString);

    if (manifestJSON.isError()) {
      return Error(manifestJSON.error());
    }

    Result<JSON::String> name = manifestJSON.get().find<JSON::String>("name");
    if (name.isNone()) {
      return Error("Failed to find \"name\" in manifest response");
    }

    Result<JSON::Array> fsLayersJSON =
      manifestJSON.get().find<JSON::Array>("fsLayers");

    if (fsLayersJSON.isNone()) {
      return Error("Failed to find \"fsLayers\" in manifest response");
    }

    Result<JSON::Array> historyArray =
      manifestJSON.get().find<JSON::Array>("history");

    if (historyArray.isNone()) {
      return Error("Failed to find \"history\" in manifest response");
    }

    if (historyArray.get().values.size() != fsLayersJSON.get().values.size()) {
      return Error(
          "\"history\" and \"fsLayers\" array count mismatch"
          "in manifest response");
    }

    vector<FileSystemLayerInfo> fsLayers;

    // We add layers in reverse order because 'fsLayers' in the manifest
    // response is ordered with the latest layer on the top. When we apply the
    // layer changes, we want the filesystem modification order to be the same
    // as its history(oldest layer applied first).
    for (size_t index = fsLayersJSON.get().values.size(); index-- > 0; ) {
      const JSON::Value& layer = fsLayersJSON.get().values[index];

      if (!layer.is<JSON::Object>()) {
        return Error(
            "Failed to parse layer as a JSON object for index: " +
            stringify(index));
      }

      const JSON::Object& layerInfoJSON = layer.as<JSON::Object>();

      // Get blobsum for layer.
      const Result<JSON::String> blobSumInfo =
        layerInfoJSON.find<JSON::String>("blobSum");

      if (blobSumInfo.isNone()) {
        return Error("Failed to find \"blobSum\" in manifest response");
      }

      // Get history for layer.
      if (!historyArray.get().values[index].is<JSON::Object>()) {
        return Error(
            "Failed to parse history as a JSON object for index: " +
            stringify(index));
      }
      const JSON::Object& historyObj =
        historyArray.get().values[index].as<JSON::Object>();

      // Get layer id.
      const Result<JSON::String> v1CompatibilityJSON =
        historyObj.find<JSON::String>("v1Compatibility");

      if (!v1CompatibilityJSON.isSome()) {
        return Error(
            "Failed to obtain layer v1 compability json in manifest for layer: "
            + stringify(index));
      }

      Try<JSON::Object> v1CompatibilityObj =
        JSON::parse<JSON::Object>(v1CompatibilityJSON.get().value);

      if (!v1CompatibilityObj.isSome()) {
        return Error(
            "Failed to parse v1 compability json in manifest for layer: "
            + stringify(index));
      }

      const Result<JSON::String> id =
        v1CompatibilityObj.get().find<JSON::String>("id");

      if (!id.isSome()) {
        return Error(
            "Failed to find \"id\" in manifest for layer: " + stringify(index));
      }

      fsLayers.emplace_back(
          FileSystemLayerInfo{
            blobSumInfo.get().value,
            id.get().value,
          });
    }

    return Manifest{name.get().value, fsLayers};
}


Future<Manifest> RegistryClientProcess::getManifest(
    const Image::Name& imageName)
{
  http::URL manifestURL(registryServer_);
  manifestURL.path =
    "v2/" + imageName.repository() + "/manifests/" + imageName.tag();

  return doHttpGet(manifestURL, None(), true, None())
    .then(defer(self(), [this] (
        const http::Response& response) -> Future<Manifest> {
      // TODO(jojy): We dont use the digest that is returned in header.
      // This is a good place to validate the manifest.

      Try<Manifest> manifest = Manifest::create(response.body);
      if (manifest.isError()) {
        return Failure(
            "Failed to parse manifest response: " + manifest.error());
      }

      return manifest.get();
    }));
}


Future<size_t> RegistryClientProcess::getBlob(
    const string& path,
    const Option<string>& digest,
    const Path& filePath)
{
  const string dirName = filePath.dirname();

  Try<Nothing> mkdir = os::mkdir(dirName, true);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create directory to download blob: " + mkdir.error());
  }

  if (strings::contains(path, " ")) {
    return Failure("Invalid repository path: " + path);
  }

  http::URL blobURL(registryServer_);
  blobURL.path =
    "v2/" + path + "/blobs/" + digest.getOrElse("");

  auto saveBlob = [filePath](const http::Response& httpResponse)
      -> Future<size_t> {
    // TODO(jojy): Add verification step.
    // TODO(jojy): Add check for max size.
    size_t size = httpResponse.body.length();
    Try<int> fd = os::open(
        filePath.value,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if (fd.isError()) {
      return Failure("Failed to open file '" + filePath.value + "': " +
                     fd.error());
    }

    return process::io::write(fd.get(), httpResponse.body)
      .then([size](const Future<Nothing>&) { return size; })
      .onAny([fd]() { os::close(fd.get()); } );
  };

  return doHttpGet(blobURL, None(), true, None())
    .then([saveBlob](const http::Response& response) {
      return saveBlob(response);
    });
}

} // namespace registry {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
