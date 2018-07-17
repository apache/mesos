// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <gtest/gtest.h>

#include <string>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>

#include <process/authenticator.hpp>
#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/profiler.hpp>

namespace authentication = process::http::authentication;
namespace http = process::http;

using authentication::Authenticator;
using authentication::BasicAuthenticator;

using http::BadRequest;
using http::OK;
using http::Response;
using http::Unauthorized;

using process::Future;
using process::READWRITE_HTTP_AUTHENTICATION_REALM;
using process::UPID;

using std::string;


// TODO(greggomann): Move this into a base class in 'mesos.hpp'.
class ProfilerTest : public ::testing::Test
{
protected:
  Future<Nothing> setAuthenticator(
      const string& realm,
      process::Owned<Authenticator> authenticator)
  {
    realms.insert(realm);

    return authentication::setAuthenticator(realm, authenticator);
  }

  void TearDown() override
  {
    foreach (const string& realm, realms) {
      // We need to wait in order to ensure that the operation
      // completes before we leave TearDown. Otherwise, we may
      // leak a mock object.
      AWAIT_READY(authentication::unsetAuthenticator(realm));
    }
    realms.clear();
  }

private:
  hashset<string> realms;
};


// Tests that the profiler's HTTP endpoints return the correct responses
// based on whether or not the profiler has been enabled.
TEST_F(ProfilerTest, StartAndStop)
{
  UPID upid("profiler", process::address());

  Future<Response> response = http::get(upid, "start");
#ifdef ENABLE_GPERFTOOLS
  Option<string> profilerEnabled = os::getenv("LIBPROCESS_ENABLE_PROFILER");

  if (profilerEnabled.isSome() && profilerEnabled.get() == "1") {
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_BODY_EQ("Profiler started.\n", response);

    response = http::get(upid, "stop");
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  } else {
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
    AWAIT_EXPECT_RESPONSE_BODY_EQ(
        "The profiler is not enabled. To enable the profiler, libprocess must "
        "be started with LIBPROCESS_ENABLE_PROFILER=1 in the environment.\n",
        response);

    response = http::get(upid, "stop");
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
    AWAIT_EXPECT_RESPONSE_BODY_EQ("Profiler not running.\n", response);
  }
#else
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Perftools is disabled. To enable perftools, "
      "configure libprocess with --enable-perftools.\n",
      response);

  response = http::get(upid, "stop");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Perftools is disabled. To enable perftools, "
      "configure libprocess with --enable-perftools.\n",
      response);
#endif
}


// Tests that the profiler's HTTP endpoints reject unauthenticated
// requests when HTTP authentication is enabled.
TEST_F(ProfilerTest, StartAndStopAuthenticationEnabled)
{
  process::Owned<Authenticator> authenticator(
    new BasicAuthenticator(
        READWRITE_HTTP_AUTHENTICATION_REALM, {{"foo", "bar"}}));

  AWAIT_READY(
      setAuthenticator(READWRITE_HTTP_AUTHENTICATION_REALM, authenticator));

  UPID upid("profiler", process::address());

  Future<Response> response = http::get(upid, "start");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

  response = http::get(upid, "stop");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
}
