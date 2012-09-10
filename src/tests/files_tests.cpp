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

#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>

#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>

#include "files/files.hpp"

#include "tests/utils.hpp"

using namespace mesos::internal;
using namespace mesos::internal::test;

using process::Future;
using process::http::BadRequest;
using process::http::InternalServerError;
using process::http::NotFound;
using process::http::OK;
using process::http::Response;
using process::http::Request;
using process::http::ServiceUnavailable;

using std::string;


void checkResponse(const Future<Response>& response,
                   const string& status,
                   const JSON::Value& expected)
{
  EXPECT_RESPONSE_STATUS_WILL_EQ(status, response);
  EXPECT_EQ(stringify(expected), response.get().body);
}


TEST_WITH_WORKDIR(FilesTest, AttachTest)
{
  Files files;
  ASSERT_TRUE(os::write("file", "body").get());
  ASSERT_TRUE(os::mkdir("dir"));

  EXPECT_FUTURE_WILL_SUCCEED(files.attach("file", "myname"));   // Valid file.
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("dir", "mydir"));     // Valid dir.
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("file", "myname"));   // Re-attach.
  EXPECT_FUTURE_WILL_FAIL(files.attach("missing", "somename")); // Missing file.

  ASSERT_TRUE(os::write("file2", "body").get());

  EXPECT_FUTURE_WILL_SUCCEED(files.attach("file2", "myname"));  // Overwrite.
  EXPECT_FUTURE_WILL_FAIL(files.attach("$@", "foo"));           // Bad path.
}


TEST_WITH_WORKDIR(FilesTest, DetachTest)
{
  Files files;

  ASSERT_TRUE(os::write("file", "body").get());
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("file", "myname"));

  files.detach("myname");
  files.detach("myname");
}


TEST_WITH_WORKDIR(FilesTest, ReadTest)
{
  Files files;
  const process::PID<>& pid = files.pid();

  ASSERT_TRUE(os::write("file", "body").get());
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("file", "/myname"));
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("file", "myname"));

  // Read a valid file.
  JSON::Object expected;
  expected.values["offset"] = 0;
  expected.values["length"] = strlen("body");
  expected.values["data"] = "body";

  checkResponse(process::http::get(pid, "read.json?path=/myname&offset=0"),
                OK().status,
                expected);
  checkResponse(process::http::get(pid, "read.json?path=myname&offset=0"),
                OK().status,
                expected);

  // Missing file.
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      NotFound().status,
      process::http::get(pid, "read.json?path=missing"));

  // Test the directory / file resolution.
  ASSERT_TRUE(os::mkdir("1/2"));
  ASSERT_TRUE(os::write("1/two", "two").get());
  ASSERT_TRUE(os::write("1/2/three", "three").get());

  // Attach some paths.
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("1", "one"));
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("1", "/one/"));
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("1/2", "two"));
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("1/2", "one/two"));

  // Resolve 1/2/3 via each attached path.
  JSON::Object expectedResponse;
  expectedResponse.values["offset"] = 0;
  expectedResponse.values["length"] = strlen("three");
  expectedResponse.values["data"] = "three";

  checkResponse(process::http::get(pid, "read.json?path=one/2/three&offset=0"),
                OK().status,
                expectedResponse);
  checkResponse(process::http::get(pid, "read.json?path=/one/2/three&offset=0"),
                OK().status,
                expectedResponse);
  checkResponse(process::http::get(pid, "read.json?path=two/three&offset=0"),
                OK().status,
                expectedResponse);
  checkResponse(
      process::http::get(pid, "read.json?path=one/two/three&offset=0"),
      OK().status,
      expectedResponse);


  // Reading dirs not allowed.
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      BadRequest().status,
      process::http::get(pid, "read.json?path=one/2"));
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      BadRequest().status,
      process::http::get(pid, "read.json?path=one"));
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      BadRequest().status,
      process::http::get(pid, "read.json?path=one/"));

  // Breaking out of sandbox
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      BadRequest().status,
      process::http::get(pid, "read.json?path=two/../two"));
}


TEST_WITH_WORKDIR(FilesTest, BrowseTest)
{
  Files files;
  const process::PID<>& pid = files.pid();

  ASSERT_TRUE(os::mkdir("1/2"));
  ASSERT_TRUE(os::mkdir("1/3"));
  ASSERT_TRUE(os::write("1/two", "two").get());
  ASSERT_TRUE(os::write("1/three", "three").get());

  EXPECT_FUTURE_WILL_SUCCEED(files.attach("1", "one"));

  // Get the listing.
  JSON::Array expected;
  expected.values.push_back(jsonFileInfo("one/2", true, 0u));
  expected.values.push_back(jsonFileInfo("one/3", true, 0u));
  expected.values.push_back(jsonFileInfo("one/three", false, 5u));
  expected.values.push_back(jsonFileInfo("one/two", false, 3u));

  checkResponse(process::http::get(pid, "browse.json?path=one/"),
                OK().status,
                expected);
  checkResponse(process::http::get(pid, "browse.json?path=one"),
                OK().status,
                expected);

  // Empty listing.
  checkResponse(process::http::get(pid, "browse.json?path=one/2"),
                OK().status,
                JSON::Array());

  // Missing dir.
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      NotFound().status,
      process::http::get(pid, "browse.json?path=missing"));
}
