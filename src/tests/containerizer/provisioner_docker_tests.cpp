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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <stout/duration.hpp>

#include <process/address.hpp>
#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>
#include <process/subprocess.hpp>

#include <process/ssl/gtest.hpp>

#include "slave/containerizer/provisioner/docker/registry_client.hpp"
#include "slave/containerizer/provisioner/docker/token_manager.hpp"

#include "tests/mesos.hpp"

using std::map;
using std::string;
using std::vector;

using namespace mesos::internal::slave::docker::registry;

using process::Clock;
using process::Future;
using process::Owned;

using process::network::Socket;

using ManifestResponse = RegistryClient::ManifestResponse;

namespace mesos {
namespace internal {
namespace tests {


/**
 * Provides token operations and defaults.
 */
class TokenHelper {
protected:
  const string hdrBase64 = base64::encode(
    "{ \
      \"alg\":\"ES256\", \
      \"typ\":\"JWT\", \
      \"x5c\":[\"test\"] \
    }");

  string getClaimsBase64() const
  {
    return base64::encode(claimsJsonString);
  }

  string getTokenString() const
  {
    return  hdrBase64 + "." + getClaimsBase64() + "." + signBase64;
  }

  string getDefaultTokenString()
  {
    // Construct response and send(server side).
    const double expirySecs = Clock::now().secs() + Days(365).secs();

    claimsJsonString =
      "{\"access\" \
        :[ \
        { \
          \"type\":\"repository\", \
            \"name\":\"library/busybox\", \
            \"actions\":[\"pull\"]}], \
            \"aud\":\"registry.docker.io\", \
            \"exp\":" + stringify(expirySecs) + ", \
            \"iat\":1438887168, \
            \"iss\":\"auth.docker.io\", \
            \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
            \"nbf\":1438887166, \
            \"sub\":\"\" \
        }";

    return getTokenString();
  }

  const string signBase64 = base64::encode("{\"\"}");
  string claimsJsonString;
};


/**
 * Fixture for testing TokenManager component.
 */
class RegistryTokenTest : public TokenHelper, public ::testing::Test
{};


// Tests JSON Web Token parsing for a valid token string.
TEST_F(RegistryTokenTest, ValidToken)
{
  const double expirySecs = Clock::now().secs() + Days(365).secs();

  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"exp\":" + stringify(expirySecs) + ", \
          \"iat\":1438887168, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":1438887166, \
          \"sub\":\"\" \
         }";

  Try<Token> token = Token::create(getTokenString());

  ASSERT_SOME(token);
}


// Tests JSON Web Token parsing for a token string with expiration date in the
// past.
TEST_F(RegistryTokenTest, ExpiredToken)
{
  const double expirySecs = Clock::now().secs() - Days(365).secs();

  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"exp\":" + stringify(expirySecs) + ", \
          \"iat\":1438887166, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":1438887166, \
          \"sub\":\"\" \
         }";

  Try<Token> token = Token::create(getTokenString());

  EXPECT_ERROR(token);
}


// Tests JSON Web Token parsing for a token string with no expiration date.
TEST_F(RegistryTokenTest, NoExpiration)
{
  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"iat\":1438887166, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":1438887166, \
          \"sub\":\"\" \
      }";

  const Try<Token> token = Token::create(getTokenString());

  ASSERT_SOME(token);
}


// Tests JSON Web Token parsing for a token string with not-before date in the
// future.
TEST_F(RegistryTokenTest, NotBeforeInFuture)
{
  const double expirySecs = Clock::now().secs() + Days(365).secs();
  const double nbfSecs = Clock::now().secs() + Days(7).secs();

  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"exp\":" + stringify(expirySecs) + ", \
          \"iat\":1438887166, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":" + stringify(nbfSecs) + ", \
          \"sub\":\"\" \
         }";

  const Try<Token> token = Token::create(getTokenString());

  ASSERT_SOME(token);
  ASSERT_EQ(token.get().isValid(), false);
}


#ifdef USE_SSL_SOCKET

// Test suite for docker registry tests.
class RegistryClientTest : public virtual SSLTest, public TokenHelper
{
protected:
  RegistryClientTest() {}

  static void SetUpTestCase()
  {
    SSLTest::SetUpTestCase();

    if (os::mkdir(RegistryClientTest::OUTPUT_DIR).isError()) {
      SSLTest::cleanup_directories();
      ABORT("Could not create temporary directory: " +
          RegistryClientTest::OUTPUT_DIR);
    }
  }

  static void TearDownTestCase()
  {
    SSLTest::TearDownTestCase();

    os::rmdir(RegistryClientTest::OUTPUT_DIR);
  }

  static const string OUTPUT_DIR;
};

const string RegistryClientTest::OUTPUT_DIR = "output_dir";

// Tests TokenManager for a simple token request.
TEST_F(RegistryClientTest, SimpleGetToken)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}});

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  // Create URL from server hostname and port.
  const process::http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  Try<Owned<TokenManager>> tokenMgr = TokenManager::create(url);
  ASSERT_SOME(tokenMgr);

  Future<Token> token =
    tokenMgr.get()->getToken(
        "registry.docker.io",
        "repository:library/busybox:pull",
        None());

  AWAIT_ASSERT_READY(socket);

  // Construct response and send(server side).
  const double expirySecs = Clock::now().secs() + Days(365).secs();

  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"exp\":" + stringify(expirySecs) + ", \
          \"iat\":1438887168, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":1438887166, \
          \"sub\":\"\" \
         }";

  const string tokenString(getTokenString());
  const string tokenResponse = "{\"token\":\"" + tokenString + "\"}";

  const string buffer =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(tokenResponse.length()) + "\r\n" +
    "\r\n" +
    tokenResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(buffer));

  AWAIT_ASSERT_READY(token);
  ASSERT_EQ(token.get().raw, tokenString);
}


// Tests TokenManager for bad token response from server.
TEST_F(RegistryClientTest, BadTokenResponse)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}});

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  // Create URL from server hostname and port.
  const process::http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  Try<Owned<TokenManager>> tokenMgr = TokenManager::create(url);
  ASSERT_SOME(tokenMgr);

  Future<Token> token =
    tokenMgr.get()->getToken(
        "registry.docker.io",
        "repository:library/busybox:pull",
        None());

  AWAIT_ASSERT_READY(socket);

  const string tokenString("bad token");
  const string tokenResponse = "{\"token\":\"" + tokenString + "\"}";

  const string buffer =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(tokenResponse.length()) + "\r\n" +
    "\r\n" +
    tokenResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(buffer));

  AWAIT_FAILED(token);
}


// Tests TokenManager for request to invalid server.
TEST_F(RegistryClientTest, BadTokenServerAddress)
{
  // Create an invalid URL with current time.
  const process::http::URL url("https", stringify(Clock::now().secs()), 0);

  Try<Owned<TokenManager>> tokenMgr = TokenManager::create(url);
  ASSERT_SOME(tokenMgr);

  Future<Token> token =
    tokenMgr.get()->getToken(
        "registry.docker.io",
        "repository:library/busybox:pull",
        None());

  AWAIT_FAILED(token);
}


// Tests docker registry's getManifest API.
TEST_F(RegistryClientTest, SimpleGetManifest)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}});

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  const process::http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  Try<Owned<RegistryClient>> registryClient =
    RegistryClient::create(url, url, None());

  ASSERT_SOME(registryClient);

  Future<ManifestResponse> manifestResponseFuture =
    registryClient.get()->getManifest("library/busybox", "latest", None());

  const string unauthResponseHeaders = "Www-Authenticate: Bearer"
    " realm=\"https://auth.docker.io/token\","
    "service=" + stringify(server.get().address().get()) + ","
    "scope=\"repository:library/busybox:pull\"";

  const string unauthHttpResponse =
    string("HTTP/1.1 401 Unauthorized\r\n") +
    unauthResponseHeaders + "\r\n" +
    "\r\n";

  AWAIT_ASSERT_READY(socket);

  // Send 401 Unauthorized response for a manifest request.
  Future<string> manifestHttpRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(manifestHttpRequestFuture);
  AWAIT_ASSERT_READY(Socket(socket.get()).send(unauthHttpResponse));

  // Token response.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  Future<string> tokenRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(tokenRequestFuture);

  const string tokenResponse =
    "{\"token\":\"" + getDefaultTokenString() + "\"}";

  const string tokenHttpResponse =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(tokenResponse.length()) + "\r\n" +
    "\r\n" +
    tokenResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(tokenHttpResponse));

  // Manifest response.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  manifestHttpRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(manifestHttpRequestFuture);

  const string manifestResponse = " \
    { \
      \"schemaVersion\": 1, \
      \"name\": \"library/busybox\", \
      \"tag\": \"latest\",  \
      \"architecture\": \"amd64\",  \
      \"fsLayers\": [ \
        { \
          \"blobSum\": \
  \"sha256:a3ed95caeb02ffe68cdd9fd84406680ae93d633cb16422d00e8a7c22955b46d4\"  \
        },  \
        { \
          \"blobSum\": \
  \"sha256:1db09adb5ddd7f1a07b6d585a7db747a51c7bd17418d47e91f901bdf420abd66\"  \
        },  \
        { \
          \"blobSum\": \
  \"sha256:a3ed95caeb02ffe68cdd9fd84406680ae93d633cb16422d00e8a7c22955b46d4\"  \
        } \
      ],  \
       \"signatures\": [  \
          { \
             \"header\": {  \
                \"jwk\": {  \
                   \"crv\": \"P-256\",  \
                   \"kid\": \
           \"OOI5:SI3T:LC7D:O7DX:FY6S:IAYW:WDRN:VQEM:BCFL:OIST:Q3LO:GTQQ\",  \
                   \"kty\": \"EC\", \
                   \"x\": \"J2N5ePGhlblMI2cdsR6NrAG_xbNC_X7s1HRtk5GXvzM\", \
                   \"y\": \"Idr-tEBjnNnfq6_71aeXBi3Z9ah_rrE209l4wiaohk0\" \
                },  \
                \"alg\": \"ES256\"  \
             }, \
             \"signature\": \
\"65vq57TakC_yperuhfefF4uvTbKO2L45gYGDs5bIEgOEarAs7_"
"4dbEV5u-W7uR8gF6EDKfowUCmTq3a5vEOJ3w\", \
       \"protected\": \
       \"eyJmb3JtYXRMZW5ndGgiOjUwNTgsImZvcm1hdFRhaWwiOiJDbjAiLCJ0aW1lIjoiMjAxNS"
       "0wOC0xMVQwMzo0Mjo1OVoifQ\"  \
          } \
       ]  \
    }";

  const string manifestHttpResponse =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(manifestResponse.length()) + "\r\n" +
    "Docker-Content-Digest: "
    "sha256:df9e13f36d2d5b30c16bfbf2a6110c45ebed0bfa1ea42d357651bc6c736d5322"
    + "\r\n" +
    "\r\n" +
    manifestResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(manifestHttpResponse));

  AWAIT_ASSERT_READY(manifestResponseFuture);
}


// Tests docker registry's getBlob API.
TEST_F(RegistryClientTest, SimpleGetBlob)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}});

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  const process::http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  Try<Owned<RegistryClient>> registryClient =
    RegistryClient::create(url, url, None());

  ASSERT_SOME(registryClient);

  const Path blobPath(RegistryClientTest::OUTPUT_DIR + "/blob");

  Future<size_t> resultFuture =
    registryClient.get()->getBlob(
        "/blob",
        "digest",
        blobPath,
        None(),
        None());

  const string unauthResponseHeaders = "WWW-Authenticate: Bearer"
    " realm=\"https://auth.docker.io/token\","
    "service=" + stringify(server.get().address().get()) + ","
    "scope=\"repository:library/busybox:pull\"";

  const string unauthHttpResponse =
    string("HTTP/1.1 401 Unauthorized\r\n") +
    unauthResponseHeaders + "\r\n" +
    "\r\n";

  AWAIT_ASSERT_READY(socket);

  // Send 401 Unauthorized response.
  Future<string> blobHttpRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(blobHttpRequestFuture);
  AWAIT_ASSERT_READY(Socket(socket.get()).send(unauthHttpResponse));

  // Send token response.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  Future<string> tokenRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(tokenRequestFuture);

  const string tokenResponse =
    "{\"token\":\"" + getDefaultTokenString() + "\"}";

  const string tokenHttpResponse =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(tokenResponse.length()) + "\r\n" +
    "\r\n" +
    tokenResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(tokenHttpResponse));

  // Send redirect.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  blobHttpRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(blobHttpRequestFuture);

  const string redirectHttpResponse =
    string("HTTP/1.1 307 Temporary Redirect\r\n") +
    "Location: https://" +
    stringify(server.get().address().get()) + "\r\n" +
    "\r\n";

  AWAIT_ASSERT_READY(Socket(socket.get()).send(redirectHttpResponse));

  // Finally send blob response.
  socket = server.get().accept();
  AWAIT_ASSERT_READY(socket);

  blobHttpRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(blobHttpRequestFuture);

  const string blobResponse = stringify(Clock::now());

  const string blobHttpResponse =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(blobResponse.length()) + "\r\n" +
    "\r\n" +
    blobResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(blobHttpResponse));

  AWAIT_ASSERT_READY(resultFuture);

  Try<string> blob = os::read(blobPath);
  ASSERT_SOME(blob);
  ASSERT_EQ(blob.get(), blobResponse);
}


TEST_F(RegistryClientTest, BadRequest)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}});

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  const process::http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  Try<Owned<RegistryClient>> registryClient =
    RegistryClient::create(url, url, None());

  ASSERT_SOME(registryClient);

  const Path blobPath(RegistryClientTest::OUTPUT_DIR + "/blob");

  Future<size_t> resultFuture =
    registryClient.get()->getBlob(
        "/blob",
        "digest",
        blobPath,
        None(),
        None());

  const string badRequestResponse =
    "{\"errors\": [{\"message\": \"Error1\" }, {\"message\": \"Error2\"}]}";

  const string badRequestHttpResponse =
    string("HTTP/1.1 400 Bad Request\r\n") +
    "Content-Length : " + stringify(badRequestResponse.length()) + "\r\n" +
    "\r\n" +
    badRequestResponse;

  AWAIT_ASSERT_READY(socket);

  // Send 400 Bad Request.
  Future<string> blobHttpRequestFuture = Socket(socket.get()).recv();
  AWAIT_ASSERT_READY(blobHttpRequestFuture);
  AWAIT_ASSERT_READY(Socket(socket.get()).send(badRequestHttpResponse));

  AWAIT_FAILED(resultFuture);

  ASSERT_TRUE(strings::contains(resultFuture.failure(), "Error1"));
  ASSERT_TRUE(strings::contains(resultFuture.failure(), "Error2"));
}

#endif // USE_SSL_SOCKET

} // namespace tests {
} // namespace internal {
} // namespace mesos {
