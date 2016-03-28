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

#include <string>

#include <gmock/gmock.h>

#include <mesos/authentication/http/basic_authenticator_factory.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>

#include <stout/tests/utils.hpp>

#include "files/files.hpp"

#include "tests/mesos.hpp"

namespace authentication = process::http::authentication;

using process::Future;
using process::Owned;

using process::http::BadRequest;
using process::http::NotFound;
using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using std::string;

using mesos::http::authentication::BasicAuthenticatorFactory;

namespace mesos {
namespace internal {
namespace tests {

class FilesTest : public TemporaryDirectoryTest
{
protected:
  void setBasicHttpAuthenticator(
      const string& realm,
      const Credentials& credentials)
  {
    Try<authentication::Authenticator*> authenticator =
      BasicAuthenticatorFactory::create(realm, credentials);

    ASSERT_SOME(authenticator);

    // Add this realm to the set of realms which will be unset during teardown.
    realms.insert(realm);

    // Pass ownership of the authenticator to libprocess.
    AWAIT_READY(authentication::setAuthenticator(
        realm,
        Owned<authentication::Authenticator>(authenticator.get())));
  }

  virtual void TearDown()
  {
    foreach (const string& realm, realms) {
      // We need to wait in order to ensure that the operation completes before
      // we leave `TearDown`. Otherwise, we may leak a mock object.
      AWAIT_READY(authentication::unsetAuthenticator(realm));
    }

    realms.clear();

    TemporaryDirectoryTest::TearDown();
  }

private:
  hashset<string> realms;
};


TEST_F(FilesTest, AttachTest)
{
  Files files;
  ASSERT_SOME(os::write("file", "body"));
  ASSERT_SOME(os::mkdir("dir"));

  AWAIT_EXPECT_READY(files.attach("file", "myname"));       // Valid file.
  AWAIT_EXPECT_READY(files.attach("dir", "mydir"));         // Valid dir.
  AWAIT_EXPECT_READY(files.attach("file", "myname"));       // Re-attach.
  AWAIT_EXPECT_FAILED(files.attach("missing", "somename")); // Missing file.

  ASSERT_SOME(os::write("file2", "body"));

  AWAIT_EXPECT_READY(files.attach("file2", "myname"));  // Overwrite.
  AWAIT_EXPECT_FAILED(files.attach("$@", "foo"));       // Bad path.
}


TEST_F(FilesTest, DetachTest)
{
  Files files;

  ASSERT_SOME(os::write("file", "body"));
  AWAIT_EXPECT_READY(files.attach("file", "myname"));

  files.detach("myname");
  files.detach("myname");
}


TEST_F(FilesTest, ReadTest)
{
  Files files;
  process::UPID upid("files", process::address());

  Future<Response> response =
    process::http::get(upid, "read");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Expecting 'path=value' in query.\n",
      response);

  response = process::http::get(upid, "read", "path=none&offset=hello");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Failed to parse offset: Failed to convert 'hello' to number.\n",
      response);

  response = process::http::get(upid, "read", "path=none&length=hello");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Failed to parse length: Failed to convert 'hello' to number.\n",
      response);

  // Now write a file.
  ASSERT_SOME(os::write("file", "body"));
  AWAIT_EXPECT_READY(files.attach("file", "/myname"));
  AWAIT_EXPECT_READY(files.attach("file", "myname"));

  // Read a valid file.
  JSON::Object expected;
  expected.values["offset"] = 0;
  expected.values["data"] = "body";

  response = process::http::get(upid, "read", "path=/myname&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response = process::http::get(upid, "read", "path=myname&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  // Missing file.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      NotFound().status,
      process::http::get(upid, "read", "path=missing"));
}


TEST_F(FilesTest, ResolveTest)
{
  Files files;
  process::UPID upid("files", process::address());

  // Test the directory / file resolution.
  ASSERT_SOME(os::mkdir("1/2"));
  ASSERT_SOME(os::write("1/two", "two"));
  ASSERT_SOME(os::write("1/2/three", "three"));

  // Attach some paths.
  AWAIT_EXPECT_READY(files.attach("1", "one"));
  AWAIT_EXPECT_READY(files.attach("1", "/one/"));
  AWAIT_EXPECT_READY(files.attach("1/2", "two"));
  AWAIT_EXPECT_READY(files.attach("1/2", "one/two"));

  // Resolve 1/2/3 via each attached path.
  JSON::Object expected;
  expected.values["offset"] = 0;
  expected.values["data"] = "three";

  Future<Response> response =
    process::http::get(upid, "read", "path=one/2/three&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response =
    process::http::get(upid, "read", "path=/one/2/three&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response =
    process::http::get(upid, "read", "path=two/three&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response =
    process::http::get(upid, "read", "path=one/two/three&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  // Percent encoded '/' urls.
  response =
    process::http::get(upid, "read", "path=%2Fone%2F2%2Fthree&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response =
    process::http::get(upid, "read", "path=one%2Ftwo%2Fthree&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  // Reading dirs not allowed.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      BadRequest().status,
      process::http::get(upid, "read", "path=one/2"));
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      BadRequest().status,
      process::http::get(upid, "read", "path=one"));
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      BadRequest().status,
      process::http::get(upid, "read", "path=one/"));

  // Breaking out of sandbox.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      BadRequest().status,
      process::http::get(upid, "read", "path=two/../two"));
}


TEST_F(FilesTest, BrowseTest)
{
  Files files;
  process::UPID upid("files", process::address());

  ASSERT_SOME(os::mkdir("1/2"));
  ASSERT_SOME(os::mkdir("1/3"));
  ASSERT_SOME(os::write("1/two", "two"));
  ASSERT_SOME(os::write("1/three", "three"));

  AWAIT_EXPECT_READY(files.attach("1", "one"));

  // Get the listing.
  struct stat s;
  JSON::Array expected;
  ASSERT_EQ(0, stat("1/2", &s));
  expected.values.push_back(jsonFileInfo("one/2", s));
  ASSERT_EQ(0, stat("1/3", &s));
  expected.values.push_back(jsonFileInfo("one/3", s));
  ASSERT_EQ(0, stat("1/three", &s));
  expected.values.push_back(jsonFileInfo("one/three", s));
  ASSERT_EQ(0, stat("1/two", &s));
  expected.values.push_back(jsonFileInfo("one/two", s));

  Future<Response> response =
      process::http::get(upid, "browse", "path=one/");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response = process::http::get(upid, "browse", "path=one%2F");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response = process::http::get(upid, "browse", "path=one");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  // Empty listing.
  response = process::http::get(upid, "browse", "path=one/2");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(JSON::Array()), response);

  // Missing dir.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      NotFound().status,
      process::http::get(upid, "browse", "path=missing"));
}


TEST_F(FilesTest, DownloadTest)
{
  Files files;
  process::UPID upid("files", process::address());

  // This is a one-pixel black gif image.
  const unsigned char gifData[] = {
      0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x01, 0x00, 0x01, 0x00, 0x91, 0x00,
      0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02,
      0x02, 0x4c, 0x01, 0x00, 0x3b, 0x00
  };
  string data((const char*) gifData, sizeof(gifData));

  ASSERT_SOME(os::write("binary", "no file extension"));
  ASSERT_SOME(os::write("black.gif", data));
  AWAIT_EXPECT_READY(files.attach("binary", "binary"));
  AWAIT_EXPECT_READY(files.attach("black.gif", "black.gif"));

  Future<Response> response =
    process::http::get(upid, "download", "path=binary");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      "application/octet-stream",
      "Content-Type",
      response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("no file extension", response);

  response = process::http::get(upid, "download", "path=black.gif");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("image/gif", "Content-Type", response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(data, response);
}


// Tests that the '/files/debug' endpoint works as expected.
TEST_F(FilesTest, DebugTest)
{
  Files files;
  process::UPID upid("files", process::address());

  ASSERT_SOME(os::mkdir("real-path-1"));
  ASSERT_SOME(os::mkdir("real-path-2"));

  AWAIT_EXPECT_READY(files.attach("real-path-1", "virtual-path-1"));
  AWAIT_EXPECT_READY(files.attach("real-path-2", "virtual-path-2"));

  // Construct the expected JSON output.
  const string cwd = os::getcwd();
  JSON::Object expected;
  expected.values["virtual-path-1"] = path::join(cwd, "real-path-1");
  expected.values["virtual-path-2"] = path::join(cwd, "real-path-2");

  Future<Response> response = process::http::get(upid, "debug");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);
}


// Tests that requests to the '/files/*' endpoints receive an `Unauthorized`
// response when HTTP authentication is enabled and an invalid credential is
// provided.
TEST_F(FilesTest, AuthenticationTest)
{
  const string AUTHENTICATION_REALM = "realm";

  Credentials credentials;
  credentials.add_credentials()->CopyFrom(DEFAULT_CREDENTIAL);

  // Create a basic HTTP authenticator with the specified credentials and set it
  // as the authenticator for `AUTHENTICATION_REALM`.
  setBasicHttpAuthenticator(AUTHENTICATION_REALM, credentials);

  // The realm is passed to `Files` to enable
  // HTTP authentication on its endpoints.
  Files files(AUTHENTICATION_REALM);

  process::UPID upid("files", process::address());

  Credential badCredential;
  badCredential.set_principal("bad-principal");
  badCredential.set_secret("bad-secret");

  const string expectedAuthorizationHeader =
    "Basic realm=\"" + AUTHENTICATION_REALM + "\"";

  Future<Response> response = process::http::get(upid, "browse");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  EXPECT_EQ(response.get().headers.at("WWW-Authenticate"),
            expectedAuthorizationHeader);

  response = process::http::get(upid, "read");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  EXPECT_EQ(response.get().headers.at("WWW-Authenticate"),
            expectedAuthorizationHeader);

  response = process::http::get(upid, "download");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  EXPECT_EQ(response.get().headers.at("WWW-Authenticate"),
            expectedAuthorizationHeader);

  response = process::http::get(upid, "debug");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  EXPECT_EQ(response.get().headers.at("WWW-Authenticate"),
            expectedAuthorizationHeader);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
