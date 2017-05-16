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

#include <list>
#include <string>
#include <tuple>
#include <vector>

#include <process/collect.hpp>
#include <process/dispatch.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>

#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/strings.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/write.hpp>

#include <mesos/docker/spec.hpp>

#include "uri/utils.hpp"

#include "uri/fetchers/docker.hpp"

#include "uri/schemes/docker.hpp"
#include "uri/schemes/http.hpp"

namespace http = process::http;
namespace io = process::io;
namespace spec = docker::spec;

using std::list;
using std::set;
using std::string;
using std::tuple;
using std::vector;

using process::dispatch;
using process::spawn;
using process::terminate;
using process::wait;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::Subprocess;

namespace mesos {
namespace uri {

//-------------------------------------------------------------------
// Helper and utility functions.
//-------------------------------------------------------------------

// Returns the set of schemes supported by this URI fetcher plugin.
static set<string> schemes()
{
  return {
    "docker",           // Fetch image manifest and blobs.
    "docker-manifest",  // Fetch image manifest only.
    "docker-blob"       // Fetch a single image blob.
  };
}

// TODO(jieyu): Move the following curl based utility functions to a
// command utils common directory.

// Uses the curl command to send an HTTP request to the given URL and
// returns the HTTP response it received. The location redirection and
// HTTPS connections will be handled automatically by the curl
// command. The returned HTTP response will have the type 'BODY' (no
// streaming).
static Future<http::Response> curl(
    const string& uri,
    const http::Headers& headers = http::Headers())
{
  vector<string> argv = {
    "curl",
    "-s",       // Don't show progress meter or error messages.
    "-S",       // Make curl show an error message if it fails.
    "-L",       // Follow HTTP 3xx redirects.
    "-i",       // Include the HTTP-header in the output.
    "--raw",    // Disable HTTP decoding of content or transfer encodings.
  };

  // Add additional headers.
  foreachpair (const string& key, const string& value, headers) {
    argv.push_back("-H");
    argv.push_back(key + ": " + value);
  }

  argv.push_back(strings::trim(uri));

  // TODO(jieyu): Kill the process if discard is called.
  Try<Subprocess> s = subprocess(
      "curl",
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to exec the curl subprocess: " + s.error());
  }

  return await(
      s.get().status(),
      io::read(s.get().out().get()),
      io::read(s.get().err().get()))
    .then([](const tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t) -> Future<http::Response> {
      const Future<Option<int>>& status = std::get<0>(t);
      if (!status.isReady()) {
        return Failure(
            "Failed to get the exit status of the curl subprocess: " +
            (status.isFailed() ? status.failure() : "discarded"));
      }

      if (status->isNone()) {
        return Failure("Failed to reap the curl subprocess");
      }

      if (status->get() != 0) {
        const Future<string>& error = std::get<2>(t);
        if (!error.isReady()) {
          return Failure(
              "Failed to perform 'curl'. Reading stderr failed: " +
              (error.isFailed() ? error.failure() : "discarded"));
        }

        return Failure("Failed to perform 'curl': " + error.get());
      }

      const Future<string>& output = std::get<1>(t);
      if (!output.isReady()) {
        return Failure(
            "Failed to read stdout from 'curl': " +
            (output.isFailed() ? output.failure() : "discarded"));
      }

      // Decode HTTP responses.
      Try<vector<http::Response>> responses =
        http::decodeResponses(output.get());

      // TODO(nfnt): If we're behing a proxy, curl will use 'HTTP
      // CONNECT tunneling' to access HTTPS. The HTTP parser will
      // put the actual response(s) to the body of the 'CONNECT'
      // response. Therefore, in that case, we'll parse the body of
      // 'CONNECT' response again. See MESOS-6010 for more details.
      bool hasProxy =
        os::getenv("https_proxy").isSome() ||
        os::getenv("HTTPS_PROXY").isSome();

      if (hasProxy && responses.isSome() && responses->size() == 1) {
        const http::Response& response = responses->back();

        if (response.code == 200 &&
            !response.headers.contains("Content-Length") &&
            response.headers.get("Transfer-Encoding") != Some("chunked")) {
          responses = http::decodeResponses(response.body);
        }
      }

      if (responses.isError()) {
        return Failure(
            "Failed to decode HTTP responses: " + responses.error() +
            "\n" + output.get());
      }

      // NOTE: We always return the last response because there might
      // be a '307 Temporary Redirect' response before that.
      return responses->back();
    });
}


static Future<http::Response> curl(
    const URI& uri,
    const http::Headers& headers = http::Headers())
{
  return curl(stringify(uri), headers);
}


// TODO(jieyu): Add a comment here.
static Future<int> download(
    const string& uri,
    const string& blobPath,
    const http::Headers& headers = http::Headers())
{
  vector<string> argv = {
    "curl",
    "-s",                 // Don't show progress meter or error messages.
    "-S",                 // Make curl show an error message if it fails.
    "-w", "%{http_code}\n%{redirect_url}", // Display HTTP response code and the redirected URL. // NOLINT(whitespace/line_length)
    "-o", blobPath        // Write output to the file.
  };

  // Add additional headers.
  foreachpair (const string& key, const string& value, headers) {
    argv.push_back("-H");
    argv.push_back(key + ": " + value);
  }

  argv.push_back(uri);

  // TODO(jieyu): Kill the process if discard is called.
  Try<Subprocess> s = subprocess(
      "curl",
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to exec the curl subprocess: " + s.error());
  }

  return await(
      s.get().status(),
      io::read(s.get().out().get()),
      io::read(s.get().err().get()))
    .then([=](const tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t) -> Future<int> {
      const Future<Option<int>>& status = std::get<0>(t);
      if (!status.isReady()) {
        return Failure(
            "Failed to get the exit status of the curl subprocess: " +
            (status.isFailed() ? status.failure() : "discarded"));
      }

      if (status->isNone()) {
        return Failure("Failed to reap the curl subprocess");
      }

      if (status->get() != 0) {
        const Future<string>& error = std::get<2>(t);
        if (!error.isReady()) {
          return Failure(
              "Failed to perform 'curl'. Reading stderr failed: " +
              (error.isFailed() ? error.failure() : "discarded"));
        }

        return Failure("Failed to perform 'curl': " + error.get());
      }

      const Future<string>& output = std::get<1>(t);
      if (!output.isReady()) {
        return Failure(
            "Failed to read stdout from 'curl': " +
            (output.isFailed() ? output.failure() : "discarded"));
      }

      vector<string> tokens = strings::tokenize(output.get(), "\n", 2);
      if (tokens.empty()) {
        return Failure("Unexpected 'curl' output: " + output.get());
      }

      // Parse the output and get the HTTP response code.
      Try<int> code = numify<int>(tokens[0]);
      if (code.isError()) {
        return Failure(
            "Unexpected HTTP response code from 'curl': " + tokens[0]);
      }

      // If there are two tokens, it means that the redirect url
      // exists in the stdout and the request to download the blob
      // is already authenticated.
      if (tokens.size() == 2) {
        // Headers are not attached because the request is already
        // authenticated.
        return download(tokens[1], blobPath);
      }

      return code.get();
    });
}


static Future<int> download(
    const URI& uri,
    const string& directory,
    const http::Headers& headers = http::Headers())
{
  const string blobPath = path::join(directory, Path(uri.path()).basename());
  return download(strings::trim(stringify(uri)), blobPath, headers);
}


// Returns the 'Basic' credential as a header for pulling an image
// from a registry, if the host of the image's repository exists in
// the docker config file, or empty if there is none.
static http::Headers getAuthHeaderBasic(
    const URI& uri,
    const hashmap<string, spec::Config::Auth>& auths)
{
  http::Headers headers;

  // NOTE: The host field of uri can be either domain or IP
  // address, which is merged in docker registry puller.
  const string registry = uri.has_port()
    ? uri.host() + ":" + stringify(uri.port())
    : uri.host();

  foreachpair (const string& key, const spec::Config::Auth& value, auths) {
    // Handle domains including 'docker.io' as a special case,
    // because the url is set differently for different version
    // of docker default registry, but all of them should depend
    // on the same default namespace 'docker.io'. Please see:
    // https://github.com/docker/docker/blob/master/registry/config.go#L34
    const bool isDocker =
      strings::contains(uri.host(), "docker.io") &&
      strings::contains(key, "docker.io");

    // Should not use 'http::URL::parse()' here, since many
    // registry domain recorded in docker config file does
    // not start with 'https://' or 'http://'. They are pure
    // domain only (e.g., 'quay.io', 'localhost:5000').
    // Please see 'ResolveAuthConfig()' in:
    // https://github.com/docker/docker/blob/master/registry/auth.go
    if (isDocker || (registry == spec::parseAuthUrl(key))) {
      if (value.has_auth()) {
        headers["Authorization"] = "Basic " + value.auth();
        break;
      }
    }
  }

  return headers;
}


static http::Headers getAuthHeaderBearer(
    const Option<string>& authToken)
{
  http::Headers headers;

  if (authToken.isSome()) {
    headers["Authorization"] = "Bearer " + authToken.get();
  }

  return headers;
}

//-------------------------------------------------------------------
// DockerFetcherPlugin implementation.
//-------------------------------------------------------------------

class DockerFetcherPluginProcess : public Process<DockerFetcherPluginProcess>
{
public:
  DockerFetcherPluginProcess(
      const hashmap<string, spec::Config::Auth>& _auths)
    : ProcessBase(process::ID::generate("docker-fetcher-plugin")),
      auths(_auths) {}

  Future<Nothing> fetch(const URI& uri, const string& directory);

private:
  Future<Nothing> _fetch(
      const URI& uri,
      const string& directory,
      const URI& manifestUri,
      const http::Headers& manifestHeaders,
      const http::Headers& basicAuthHeaders,
      const http::Response& response);

  Future<Nothing> __fetch(
      const URI& uri,
      const string& directory,
      const http::Headers& authHeaders,
      const http::Response& response);

  Future<Nothing> fetchBlob(
      const URI& uri,
      const string& directory,
      const http::Headers& authHeaders);

  Future<Nothing> _fetchBlob(
      const URI& uri,
      const string& directory,
      const URI& blobUri,
      const http::Headers& basicAuthHeaders);

  Future<Nothing> __fetchBlob(int code);

  // Returns a token-based authorization header. Basic authorization
  // header may be required to get a proper authorization token.
  Future<http::Headers> getAuthHeader(
      const URI& uri,
      const http::Headers& basicAuthHeaders,
      const http::Response& response);

  URI getManifestUri(const URI& uri);
  URI getBlobUri(const URI& uri);

  // This is a lookup table for credentials in docker config file,
  // keyed by registry URL.
  // For example, "https://index.docker.io/v1/" -> spec::Config::Auth
  hashmap<string, spec::Config::Auth> auths;
};


DockerFetcherPlugin::Flags::Flags()
{
  add(&Flags::docker_config,
      "docker_config",
      "The default docker config file.");
}


const char DockerFetcherPlugin::NAME[] = "docker";


Try<Owned<Fetcher::Plugin>> DockerFetcherPlugin::create(const Flags& flags)
{
  // TODO(jieyu): Make sure curl is available.

  hashmap<string, spec::Config::Auth> auths;
  if (flags.docker_config.isSome()) {
    Try<hashmap<string, spec::Config::Auth>> cachedAuths =
      spec::parseAuthConfig(flags.docker_config.get());

    if (cachedAuths.isError()) {
      return Error("Failed to parse docker config: " + cachedAuths.error());
    }

    auths = cachedAuths.get();
  }

  Owned<DockerFetcherPluginProcess> process(new DockerFetcherPluginProcess(
      hashmap<string, spec::Config::Auth>(auths)));

  return Owned<Fetcher::Plugin>(new DockerFetcherPlugin(process));
}


DockerFetcherPlugin::DockerFetcherPlugin(
    Owned<DockerFetcherPluginProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


DockerFetcherPlugin::~DockerFetcherPlugin()
{
  terminate(process.get());
  wait(process.get());
}


set<string> DockerFetcherPlugin::schemes() const
{
  // Use uri:: prefix to disambiguate.
  return uri::schemes();
}


string DockerFetcherPlugin::name() const
{
  return NAME;
}


Future<Nothing> DockerFetcherPlugin::fetch(
    const URI& uri,
    const string& directory) const
{
  return dispatch(
      process.get(),
      &DockerFetcherPluginProcess::fetch,
      uri,
      directory);
}


Future<Nothing> DockerFetcherPluginProcess::fetch(
    const URI& uri,
    const string& directory)
{
  // TODO(gilbert): Convert the `uri` to ::docker::spec::ImageReference
  // and pass it all the way down to avoid the complicated URI conversion
  // and make the code more readable.
  if (schemes().count(uri.scheme()) == 0) {
    return Failure(
        "Docker fetcher plugin does not support "
        "'" + uri.scheme() + "' URI scheme");
  }

  if (!uri.has_host()) {
    return Failure("Registry host (uri.host) is not specified");
  }

  if (!uri.has_query()) {
    return Failure("Image tag/digest (uri.query) is not specified");
  }

  Try<Nothing> mkdir = os::mkdir(directory);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create directory '" +
        directory + "': " + mkdir.error());
  }

  // Use the 'Basic' credential to pull the manifest/blob by default.
  http::Headers basicAuthHeaders = getAuthHeaderBasic(uri, auths);

  if (uri.scheme() == "docker-blob") {
    return fetchBlob(uri, directory, basicAuthHeaders);
  }

  URI manifestUri = getManifestUri(uri);

  // Request a Version 2 Schema 1 manifest. The MIME type of a Schema 1
  // manifest is described in the following link:
  // https://docs.docker.com/registry/spec/manifest-v2-1/
  // Note: The 'Accept' header is required for Amazon ECR. See:
  // https://forums.aws.amazon.com/message.jspa?messageID=780440
  http::Headers manifestHeaders = {
    {"Accept", "application/vnd.docker.distribution.manifest.v1+json"}
  };

  return curl(manifestUri, manifestHeaders + basicAuthHeaders)
    .then(defer(self(),
                &Self::_fetch,
                uri,
                directory,
                manifestUri,
                manifestHeaders,
                basicAuthHeaders,
                lambda::_1));
}


Future<Nothing> DockerFetcherPluginProcess::_fetch(
    const URI& uri,
    const string& directory,
    const URI& manifestUri,
    const http::Headers& manifestHeaders,
    const http::Headers& basicAuthHeaders,
    const http::Response& response)
{
  if (response.code == http::Status::UNAUTHORIZED) {
    // Use the 'Basic' credential to request an auth token by default.
    return getAuthHeader(manifestUri, basicAuthHeaders, response)
      .then(defer(self(), [=](
          const http::Headers& authHeaders) -> Future<Nothing> {
        return curl(manifestUri, manifestHeaders + authHeaders)
          .then(defer(self(),
                      &Self::__fetch,
                      uri,
                      directory,
                      authHeaders,
                      lambda::_1));
      }));
  }

  return __fetch(uri, directory, basicAuthHeaders, response);
}


Future<Nothing> DockerFetcherPluginProcess::__fetch(
    const URI& uri,
    const string& directory,
    const http::Headers& authHeaders,
    const http::Response& response)
{
  if (response.code != http::Status::OK) {
    return Failure(
        "Unexpected HTTP response '" + response.status + "' "
        "when trying to get the manifest");
  }

  CHECK_EQ(response.type, http::Response::BODY);

  // Check if we got a V2 Schema 1 manifest.
  // TODO(ipronin): We have to support Schema 2 manifests to be able to use
  // digests for pulling images that were pushed with Docker 1.10+ to
  // Registry 2.3+.
  Option<string> contentType = response.headers.get("Content-Type");
  if (contentType.isSome()) {
    // NOTE: Docker support the following three media type for V2
    // schema 1 manifest:
    // 1. application/vnd.docker.distribution.manifest.v1+json
    // 2. application/vnd.docker.distribution.manifest.v1+prettyjws
    // 3. application/json
    // For more details, see:
    // https://docs.docker.com/registry/spec/manifest-v2-1/
    bool isV2Schema1 =
      strings::startsWith(
          contentType.get(),
          "application/vnd.docker.distribution.manifest.v1") ||
      strings::startsWith(
          contentType.get(),
          "application/json");

    if (!isV2Schema1) {
      return Failure("Unsupported manifest MIME type: " + contentType.get());
    }
  }

  Try<spec::v2::ImageManifest> manifest = spec::v2::parse(response.body);
  if (manifest.isError()) {
    return Failure("Failed to parse the image manifest: " + manifest.error());
  }

  // Save manifest to 'directory'.
  Try<Nothing> write = os::write(
      path::join(directory, "manifest"),
      response.body);

  if (write.isError()) {
    return Failure(
        "Failed to write the image manifest to "
        "'" + directory + "': " + write.error());
  }

  // No need to proceed if we only want manifest.
  if (uri.scheme() == "docker-manifest") {
    return Nothing();
  }

  // Download all the filesystem layers.
  list<Future<Nothing>> futures;
  for (int i = 0; i < manifest->fslayers_size(); i++) {
    URI blob = uri::docker::blob(
        uri.path(),                         // The 'repository'.
        manifest->fslayers(i).blobsum(),    // The 'digest'.
        uri.host(),                         // The 'registry'.
        (uri.has_fragment()                 // The 'scheme'.
          ? Option<string>(uri.fragment())
          : None()),
        (uri.has_port()                     // The 'port'.
          ? Option<int>(uri.port())
          : None()));

    // Use the same 'authHeaders' as for the manifest to pull the blobs.
    futures.push_back(fetchBlob(
        blob,
        directory,
        authHeaders));
  }

  return collect(futures)
    .then([]() -> Future<Nothing> { return Nothing(); });
}


Future<Nothing> DockerFetcherPluginProcess::fetchBlob(
    const URI& uri,
    const string& directory,
    const http::Headers& authHeaders)
{
  URI blobUri = getBlobUri(uri);

  return download(blobUri, directory, authHeaders)
    .then(defer(self(), [=](int code) -> Future<Nothing> {
      if (code == http::Status::UNAUTHORIZED) {
        // If we get a '401 Unauthorized', we assume that 'authHeaders'
        // is either empty or contains the 'Basic' credential, and we
        // can use it to request an auth token.
        // TODO(chhsiao): What if 'authHeaders' has an expired token?
        return _fetchBlob(uri, directory, blobUri, authHeaders);
      }

      return __fetchBlob(code);
    }));
}


Future<Nothing> DockerFetcherPluginProcess::_fetchBlob(
    const URI& uri,
    const string& directory,
    const URI& blobUri,
    const http::Headers& basicAuthHeaders)
{
  // TODO(jieyu): This extra 'curl' call can be avoided if we can get
  // HTTP headers from 'download'. Currently, 'download' only returns
  // the HTTP response code because we don't support parsing HTTP
  // headers alone. Revisit this once that's supported.
  return curl(blobUri, basicAuthHeaders)
    .then(defer(self(), [=](const http::Response& response) -> Future<Nothing> {
      // We expect a '401 Unauthorized' response here since the
      // 'download' with the same URI returns a '401 Unauthorized'.
      if (response.code != http::Status::UNAUTHORIZED) {
        return Failure(
          "Expecting a '401 Unauthorized' response when fetching a blob, "
          "but get '" + response.status + "' instead");
      }

      return getAuthHeader(blobUri, basicAuthHeaders, response)
        .then(defer(self(), [=](
            const http::Headers& authHeaders) -> Future<Nothing> {
          return download(blobUri, directory, authHeaders)
            .then(defer(self(),
                        &Self::__fetchBlob,
                        lambda::_1));
        }));
    }));
}


Future<Nothing> DockerFetcherPluginProcess::__fetchBlob(int code)
{
  if (code == http::Status::OK) {
    return Nothing();
  }

  return Failure(
      "Unexpected HTTP response '" + http::Status::string(code) + "' "
      "when trying to download the blob");
}


Future<http::Headers> DockerFetcherPluginProcess::getAuthHeader(
    const URI& uri,
    const http::Headers& basicAuthHeaders,
    const http::Response& response)
{
  Result<http::header::WWWAuthenticate> header =
    response.headers.get<http::header::WWWAuthenticate>();

  if (header.isError()) {
    return Failure(
        "Failed to get WWW-Authenticate header: " + header.error());
  } else if (header.isNone()) {
    return Failure("Unexpected empty WWW-Authenticate header");
  }

  // According to RFC, auth scheme should be case insensitive.
  const string authScheme = strings::upper(header->authScheme());

  // If a '401 Unauthorized' response is received and the auth-scheme
  // is 'Bearer', we expect a header 'Www-Authenticate' containing the
  // auth server information. We extract the auth server information
  // from the auth-param, and then contacts the auth server to get the
  // token. The token will then be placed in the subsequent HTTP
  // requests as a header.
  //
  // See details here:
  // https://docs.docker.com/registry/spec/auth/token/
  if (authScheme == "BEARER") {
    hashmap<string, string> authParam = header->authParam();

    // `authParam` is supposed to contain the 'realm', 'service'
    // and 'scope' information for bearer authentication.
    if (!authParam.contains("realm")) {
      return Failure("Missing 'realm' in WWW-Authenticate header");
    }

    if (!authParam.contains("service")) {
      return Failure("Missing 'service' in WWW-Authenticate header");
    }

    if (!authParam.contains("scope")) {
      return Failure("Missing 'scope' in WWW-Authenticate header");
    }

    // TODO(jieyu): Currently, we don't expect the auth server to return
    // a service or a scope that needs encoding.
    string authServerUri =
      authParam.at("realm") + "?" +
      "service=" + authParam.at("service") + "&" +
      "scope=" + authParam.at("scope");

    return curl(authServerUri, basicAuthHeaders)
      .then([authServerUri](
          const http::Response& response) -> Future<http::Headers> {
        if (response.code != http::Status::OK) {
          return Failure(
            "Unexpected HTTP response '" + response.status + "' "
            "when trying to GET '" + authServerUri + "'");
        }

        CHECK_EQ(response.type, http::Response::BODY);

        Try<JSON::Object> object = JSON::parse<JSON::Object>(response.body);
        if (object.isError()) {
          return Failure("Parsing the JSON object failed: " + object.error());
        }

        Result<JSON::String> token = object->find<JSON::String>("token");
        if (token.isError()) {
          return Failure(
              "Finding token in JSON object failed: " + token.error());
        } else if (token.isNone()) {
          return Failure("Failed to find token in JSON object");
        }

        return getAuthHeaderBearer(token->value);
      });
  }

  return Failure("Unsupported auth-scheme: " + authScheme);
}


URI DockerFetcherPluginProcess::getManifestUri(const URI& uri)
{
  string scheme = "https";
  if (uri.has_fragment()) {
    scheme = uri.fragment();
  }

  return uri::construct(
      scheme,
      path::join("/v2", uri.path(), "manifests", uri.query()),
      uri.host(),
      (uri.has_port() ? Option<int>(uri.port()) : None()));
}


URI DockerFetcherPluginProcess::getBlobUri(const URI& uri)
{
  string scheme = "https";
  if (uri.has_fragment()) {
    scheme = uri.fragment();
  }

  return uri::construct(
      scheme,
      path::join("/v2", uri.path(), "blobs", uri.query()),
      uri.host(),
      (uri.has_port() ? Option<int>(uri.port()) : None()));
}

} // namespace uri {
} // namespace mesos {
