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

namespace http = process::http;
namespace spec = docker::spec;

using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;

using http::Pipe;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace registry {


class RegistryClientProcess : public Process<RegistryClientProcess>
{
public:
  static Try<Owned<RegistryClientProcess>> create(
      const http::URL& registry,
      const http::URL& authenticationServer,
      const Option<Credentials>& credentials);

  Future<spec::v2::ImageManifest> getManifest(
      const spec::ImageReference& reference);

  Future<size_t> getBlob(
      const spec::ImageReference& reference,
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
      bool isStreaming,
      bool resend,
      const Option<string>& lastResponse) const;

  Try<http::Headers> getAuthenticationAttributes(
      const http::Response& httpResponse) const;

  Future<string> handleHttpBadResponse(
      const http::Response& httpResponse,
      bool isStreaming) const;

  Future<http::Response> handleHttpUnauthResponse(
      const http::Response& httpResponse,
      const http::URL& url,
      bool isStreaming) const;

  Future<http::Response> handleHttpRedirect(
      const http::Response& httpResponse,
      const Option<http::Headers>& headers,
      bool isStreaming) const;

  Future<size_t> saveBlob(
      int fd,
      Pipe::Reader reader);

  string getRepositoryPath(const spec::ImageReference& reference) const;

  string getAPIVersion() const;

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


Future<spec::v2::ImageManifest> RegistryClient::getManifest(
    const spec::ImageReference& reference)
{
  return dispatch(
      process_.get(),
      &RegistryClientProcess::getManifest,
      reference);
}


Future<size_t> RegistryClient::getBlob(
    const spec::ImageReference& reference,
    const Option<string>& digest,
    const Path& filePath)
{
  return dispatch(
        process_.get(),
        &RegistryClientProcess::getBlob,
        reference,
        digest,
        filePath);
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
    const http::URL& url,
    bool isStreaming) const
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
          isStreaming,
          true,
          httpResponse.status);
    }));
}

// TODO(jojy): Move this up to http namespace in libprocess.
// Polls the reader and passed data on each read to the input function.
// Returns the total number of bytes read.
Future<size_t> readStreamingResponse(
    Pipe::Reader reader,
    std::function<Future<Nothing>(const string&)> function,
    size_t totalSize)
{
  return reader.read()
    .then([reader, function, totalSize](
        const string& data) -> Future<size_t> {
      if (data.empty()) {
          return totalSize;
      }

      size_t length = data.length();
      return function(data)
        .then([reader, function, length, totalSize]() -> Future<size_t> {
          size_t newSize = totalSize + length;

          return readStreamingResponse(
              reader,
              function,
              newSize);
        });
    });
}


Future<string> _processErrorBody(const string& errorBody)
{
  Try<JSON::Object> errorResponse =
    JSON::parse<JSON::Object>(errorBody);

  if (errorResponse.isError()) {
    return Failure(
        "Failed to parse bad request response JSON: " +
        errorResponse.error());
  }

  std::ostringstream out;
  bool first = true;
  Result<JSON::Array> errorObjects =
    errorResponse.get().find<JSON::Array>("errors");

  if (errorObjects.isError()) {
    return Failure(
        "Failed to find 'errors' in bad request response: " +
        errorObjects.error());
  } else if (errorObjects.isNone()) {
    return Failure("Errors not found in bad request response");
  }

  foreach (const JSON::Value& error, errorObjects.get().values) {
    if (!error.is<JSON::Object>()) {
      LOG(WARNING) << "Failed to parse error message: "
                   << "'error' expected to be JSON object";

      continue;
    }

    Result<JSON::String> message =
      error.as<JSON::Object>().find<JSON::String>("message");

    if (message.isError()) {
      return Failure(
          "Failed to parse bad request error message: " +
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


Future<string> RegistryClientProcess::handleHttpBadResponse(
    const http::Response& httpResponse,
    bool isStreaming) const
{
  if (isStreaming) {
    std::shared_ptr<string> errorBody = std::make_shared<string>();

    size_t size = 0;

    auto appendResponse = [errorBody](
        const string& data) mutable -> Future<Nothing> {
      *errorBody += data;

      return Nothing();
    };

    Option<Pipe::Reader> reader = httpResponse.reader;
    if (reader.isNone()) {
      return Failure("Failed to get piped reader from streaming response");
    }

    return readStreamingResponse(reader.get(), appendResponse, size)
      .then([errorBody](size_t length) {
          return _processErrorBody(*errorBody);
      });
  }

  return _processErrorBody(httpResponse.body);
}


Future<http::Response> RegistryClientProcess::handleHttpRedirect(
    const http::Response& httpResponse,
    const Option<http::Headers>& headers,
    bool isStreaming) const
{
  if (httpResponse.headers.find("Location") ==
      httpResponse.headers.end()) {
    return Failure(
        "Invalid redirect response: 'Location' not found in headers.");
  }

  const string& location = httpResponse.headers.at("Location");
  Try<http::URL> locationUrl = http::URL::parse(location);
  if (locationUrl.isError()) {
    return Failure(
        "Failed to parse '" + location + "': " + locationUrl.error());
  }

  if (locationUrl.get().scheme.isNone()) {
    return Failure("No scheme found in redirect location");
  } else if (locationUrl.get().scheme.get() != "https") {
    return Failure(
        "Unexpected scheme '" + locationUrl.get().scheme.get() +
        "' found in redirect location");
  }

  return doHttpGet(
      locationUrl.get(),
      headers,
      isStreaming,
      false,
      httpResponse.status);
}


Future<http::Response> RegistryClientProcess::doHttpGet(
    const http::URL& url,
    const Option<http::Headers>& headers,
    bool isStreaming,
    bool resend,
    const Option<string>& lastResponseStatus) const
{
  Future<http::Response> response;

  if (isStreaming) {
    response = process::http::streaming::get(url, headers);
  } else {
    response = process::http::get(url, headers);
  }

  return response
    .then(defer(self(), [=](const http::Response& httpResponse)
        -> Future<http::Response> {
      VLOG(1) << "Response status for url '" << url << "': "
              << httpResponse.status;

      // Set the future if we get a OK response.
      if (httpResponse.status == "200 OK") {
        return httpResponse;
      }

      if (httpResponse.status == "400 Bad Request") {
        return handleHttpBadResponse(httpResponse, isStreaming)
          .then([](const string& errorResponse) -> Future<http::Response> {
            return Failure(errorResponse);
          });
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
        return handleHttpUnauthResponse(
            httpResponse,
            url,
            isStreaming);
      }

      // Handle redirect.
      if (httpResponse.status == "307 Temporary Redirect") {
        return handleHttpRedirect(httpResponse, headers, isStreaming);
      }

      return Failure("Invalid response: " + httpResponse.status);
    }));
}


// TODO(tnachen): Support other Docker registry API versions.
string RegistryClientProcess::getAPIVersion() const
{
  return "v2";
}


// Returns the path to the repository that is used to construct
// URL to call the Docker registry server.
// Currently we only support offical repositories that always prefix
// repository with library/.
// TODO(tnachen): Support unoffical repositories and loading in repository
// information.
string RegistryClientProcess::getRepositoryPath(
    const spec::ImageReference& reference) const
{
  return getAPIVersion() + "/library/" + reference.repository();
}


Future<spec::v2::ImageManifest> RegistryClientProcess::getManifest(
    const spec::ImageReference& reference)
{
  http::URL manifestURL(registryServer_);
  manifestURL.path =
    getRepositoryPath(reference) + "/manifests/" + reference.tag();

  return doHttpGet(manifestURL, None(), false, true, None())
    .then(defer(self(), [this] (
        const http::Response& response) -> Future<spec::v2::ImageManifest> {
      // TODO(jojy): We dont use the digest that is returned in header.
      // This is a good place to validate the manifest.

      Try<spec::v2::ImageManifest> manifest =
        spec::v2::parse(JSON::parse<JSON::Object>(response.body).get());
      if (manifest.isError()) {
        return Failure(
            "Failed to parse manifest response: " + manifest.error());
      }

      // We reverse the order of fsLayers and history because in
      // manifest response the latest layer is on the top.
      // We assume fslayers and history has the same length as
      // it's already checked in spec::parse.
      for (int i = 0; i < manifest.get().fslayers_size() / 2; i++) {
        manifest.get().mutable_fslayers()->SwapElements(i,
          manifest.get().fslayers_size() - 1 - i);
        manifest.get().mutable_history()->SwapElements(i,
          manifest.get().fslayers_size() - 1 - i);
      }

      return manifest.get();
    }));
}

// We are not using process::write because of an issue related to junk
// characters being written to the file (see MESOS-3798).
// TODO(jojy): Replace this with process::write once MESOS-3798 is resolved.
Future<Nothing> _saveBlob(
    int fd,
    const Owned<string>& data,
    size_t index)
{
  return process::io::write(
      fd,
      (void*) (data->data() + index),
      data->size() - index)
    .then([=](size_t writeSize) -> Future<Nothing> {
      if (index + writeSize < data->size()) {
        return _saveBlob(fd, data, index + writeSize);
      }

      return Nothing();
    });
}


Future<size_t> RegistryClientProcess::saveBlob(
    int fd,
    Pipe::Reader reader)
{
  auto writeBlob = [fd](const string& data) {
    return _saveBlob(fd, Owned<string>(new string(data)), 0);
  };

  return readStreamingResponse(reader, writeBlob, 0);
}


Future<size_t> RegistryClientProcess::getBlob(
    const spec::ImageReference& reference,
    const Option<string>& digest,
    const Path& filePath)
{
  Try<Nothing> mkdir = os::mkdir(filePath.dirname(), true);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create directory to download blob: " + mkdir.error());
  }

  const string blobURLPath = getRepositoryPath(reference) + "/blobs/" +
                             digest.getOrElse("");

  http::URL blobURL(registryServer_);
  blobURL.path = blobURLPath;

  return doHttpGet(blobURL, None(), true, true, None())
    .then(defer(self(), [this, blobURLPath, digest, filePath](
        const http::Response& response) -> Future<size_t> {
      Try<int> fd = os::open(
          filePath.value,
          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
          S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

      if (fd.isError()) {
        return Failure(
            "Failed to open file '" + filePath.value + "': " +
            fd.error());
      }

      Try<Nothing> nonblock = os::nonblock(fd.get());
      if (nonblock.isError()) {
        Try<Nothing> close = os::close(fd.get());
        if (close.isError()) {
          LOG(WARNING) << "Failed to close the file descriptor for file '"
                       << stringify(filePath) << "': " << close.error();
        }

        return Failure(
            "Failed to set non-blocking mode for file: " + filePath.value);
      }

      // TODO(jojy): Add blob validation.
      // TODO(jojy): Add check for max size.

      Option<Pipe::Reader> reader = response.reader;
      if (reader.isNone()) {
        Try<Nothing> close = os::close(fd.get());
        if (close.isError()) {
          LOG(WARNING) << "Failed to close the file descriptor for file '"
                       << stringify(filePath) << "': " << close.error();
        }

        return Failure("Failed to get streaming reader from blob response");
      }

      return saveBlob(fd.get(), reader.get())
        .onAny([blobURLPath, digest, filePath, fd](
            const Future<size_t>& future) {
          Try<Nothing> close = os::close(fd.get());
          if (close.isError()) {
            LOG(WARNING) << "Failed to close the file descriptor for blob '"
                         << stringify(filePath) << "': " << close.error();
          }

          if (future.isFailed()) {
            LOG(WARNING) << "Failed to save blob requested from '"
                         << blobURLPath << "' to path '"
                         << stringify(filePath) << "': " << future.failure();
          }

          if (future.isDiscarded()) {
            LOG(WARNING) << "Failed to save blob requested from '"
                         << blobURLPath << "' to path '" << stringify(filePath)
                         << "': future discarded";
          }
        });
    }));
}

} // namespace registry {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
