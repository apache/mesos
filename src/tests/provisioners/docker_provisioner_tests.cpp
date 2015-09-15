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

#include "common/process_dispatcher.hpp"
#include "slave/containerizer/provisioners/docker/token_manager.hpp"

#include "tests/mesos.hpp"

using std::map;
using std::string;
using std::vector;

using namespace mesos::internal::slave::docker::registry;
using namespace process;

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

  const string signBase64 = base64::encode("{\"\"}");
  string claimsJsonString;
};


/**
 * Fixture for testing TokenManager component.
 */
class DockerRegistryTokenTest : public TokenHelper, public ::testing::Test
{};


// Tests JSON Web Token parsing for a valid token string.
TEST_F(DockerRegistryTokenTest, ValidToken)
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
TEST_F(DockerRegistryTokenTest, ExpiredToken)
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
TEST_F(DockerRegistryTokenTest, NoExpiration)
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
TEST_F(DockerRegistryTokenTest, NotBeforeInFuture)
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
class DockerRegistryClientTest : public virtual SSLTest, public TokenHelper
{
protected:
  DockerRegistryClientTest() {}

  static void SetUpTestCase()
  {
    SSLTest::SetUpTestCase();
    // TODO(jojy): Add registry specific directory setup. Will be added in the
    // next patch when docker registry client tests are added.
  }

  static void TearDownTestCase()
  {
    SSLTest::TearDownTestCase();
    // TODO(jojy): Add registry specific directory cleanup. Will be added in the
    // next patch when docker registry client tests are added.
  }
};


// Tests TokenManager for a simple token request.
TEST_F(DockerRegistryClientTest, SimpleGetToken)
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
  const http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  auto tokenManagerProcess = TokenManagerProcess::create(url);
  ASSERT_SOME(tokenManagerProcess);

  Shared<TokenManager> tmProcess(tokenManagerProcess.get().release());
  auto tokenManager =
    ProcessDispatcher<TokenManager>::create(tmProcess);

  Future<Token> token =
    tokenManager.get()->dispatch(
        &TokenManager::getToken,
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


TEST_F(DockerRegistryClientTest, TokenManagerInterface)
{
  class AnotherTokenManager :
    public TokenManager,
    public Process<AnotherTokenManager>
  {
  public:
    static Owned<AnotherTokenManager> create(const string& str)
    {
      return Owned<AnotherTokenManager>(new AnotherTokenManager(str));
    }

    AnotherTokenManager(const string str)
      :str_(str) {}

    process::Future<Token> getToken(
        const std::string& service,
        const std::string& scope,
        const Option<std::string>& account)
    {
      return Failure("AnotherTokenManager");
    }

  private:
    const string str_;
  };

  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}});

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  // Create URL from server hostname and port.
  const http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  vector<Owned<Dispatchable<TokenManager>>> tokenManagerList;

  Try<Owned<Dispatchable<TokenManager>>> tokenManagerProcess1 =
    ProcessDispatcher<TokenManager, TokenManagerProcess>::create(url);
  ASSERT_SOME(tokenManagerProcess1);


  Try<Owned<Dispatchable<TokenManager>>> tokenManagerProcess2 =
    ProcessDispatcher<TokenManager, AnotherTokenManager>::create("test");

  ASSERT_SOME(tokenManagerProcess2);

  tokenManagerList.push_back(tokenManagerProcess1.get());
  tokenManagerList.push_back(tokenManagerProcess2.get());

  foreach (Owned<Dispatchable<TokenManager>>& i, tokenManagerList) {
      i->dispatch(
        &TokenManager::getToken,
        "registry.docker.io",
        "repository:library/busybox:pull",
        None());
  }
}


TEST_F(DockerRegistryClientTest, DispatchOwnsTokenManager)
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
  const http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  auto tokenManager =
    ProcessDispatcher<TokenManager, TokenManagerProcess>::create(url);

  Future<Token> token =
    tokenManager.get()->dispatch(
        &TokenManager::getToken,
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
TEST_F(DockerRegistryClientTest, BadTokenResponse)
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
  const http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  auto tokenMgr =
    ProcessDispatcher<TokenManager, TokenManagerProcess>::create(url);
  ASSERT_SOME(tokenMgr);

  Future<Token> token =
    tokenMgr.get()->dispatch(
        &TokenManager::getToken,
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
TEST_F(DockerRegistryClientTest, BadTokenServerAddress)
{
  // Create an invalid URL with current time.
  const http::URL url("https", stringify(Clock::now().secs()), 0);

  auto tokenMgr =
    ProcessDispatcher<TokenManager, TokenManagerProcess>::create(url);
  ASSERT_SOME(tokenMgr);

  Future<Token> token =
    tokenMgr.get()->dispatch(
        &TokenManager::getToken,
        "registry.docker.io",
        "repository:library/busybox:pull",
        None());

  AWAIT_FAILED(token);
}

#endif // USE_SSL_SOCKET

} // namespace tests {
} // namespace internal {
} // namespace mesos {
