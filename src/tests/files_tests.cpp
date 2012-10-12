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

#include <string>

#include <gmock/gmock.h>

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
using process::http::NotFound;
using process::http::OK;
using process::http::Response;

using std::string;


TEST_WITH_WORKDIR(FilesTest, AttachTest)
{
  Files files;
  ASSERT_TRUE(os::write("file", "body").isSome());
  ASSERT_TRUE(os::mkdir("dir").isSome());

  EXPECT_FUTURE_WILL_SUCCEED(files.attach("file", "myname"));   // Valid file.
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("dir", "mydir"));     // Valid dir.
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("file", "myname"));   // Re-attach.
  EXPECT_FUTURE_WILL_FAIL(files.attach("missing", "somename")); // Missing file.

  ASSERT_TRUE(os::write("file2", "body").isSome());

  EXPECT_FUTURE_WILL_SUCCEED(files.attach("file2", "myname"));  // Overwrite.
  EXPECT_FUTURE_WILL_FAIL(files.attach("$@", "foo"));           // Bad path.
}


TEST_WITH_WORKDIR(FilesTest, DetachTest)
{
  Files files;

  ASSERT_TRUE(os::write("file", "body").isSome());
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("file", "myname"));

  files.detach("myname");
  files.detach("myname");
}


TEST_WITH_WORKDIR(FilesTest, ReadTest)
{
  Files files;
  const process::PID<>& pid = files.pid();

  Future<Response> response =
    process::http::get(pid, "read.json");

  EXPECT_RESPONSE_STATUS_WILL_EQ(BadRequest().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ("Expecting 'path=value' in query.\n", response);

  response = process::http::get(pid, "read.json?path=none&offset=hello");

  EXPECT_RESPONSE_STATUS_WILL_EQ(BadRequest().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(
      "Failed to parse offset: Failed to convert 'hello' to number.\n",
      response);

  response = process::http::get(pid, "read.json?path=none&length=hello");

  EXPECT_RESPONSE_STATUS_WILL_EQ(BadRequest().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(
      "Failed to parse length: Failed to convert 'hello' to number.\n",
      response);

  // Now write a file.
  ASSERT_TRUE(os::write("file", "body").isSome());
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("file", "/myname"));
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("file", "myname"));

  // Read a valid file.
  JSON::Object expected;
  expected.values["offset"] = 0;
  expected.values["length"] = strlen("body");
  expected.values["data"] = "body";

  response = process::http::get(pid, "read.json?path=/myname&offset=0");

  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(stringify(expected), response);

  response = process::http::get(pid, "read.json?path=myname&offset=0");

  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(stringify(expected), response);

  // Missing file.
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      NotFound().status,
      process::http::get(pid, "read.json?path=missing"));
}


TEST_WITH_WORKDIR(FilesTest, ResolveTest)
{
  Files files;
  const process::PID<>& pid = files.pid();

  // Test the directory / file resolution.
  ASSERT_TRUE(os::mkdir("1/2").isSome());
  ASSERT_TRUE(os::write("1/two", "two").isSome());
  ASSERT_TRUE(os::write("1/2/three", "three").isSome());

  // Attach some paths.
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("1", "one"));
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("1", "/one/"));
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("1/2", "two"));
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("1/2", "one/two"));

  // Resolve 1/2/3 via each attached path.
  JSON::Object expected;
  expected.values["offset"] = 0;
  expected.values["length"] = strlen("three");
  expected.values["data"] = "three";

  Future<Response> response =
    process::http::get(pid, "read.json?path=one/2/three&offset=0");

  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(stringify(expected), response);

  response = process::http::get(pid, "read.json?path=/one/2/three&offset=0");

  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(stringify(expected), response);

  response = process::http::get(pid, "read.json?path=two/three&offset=0");

  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(stringify(expected), response);

  response = process::http::get(pid, "read.json?path=one/two/three&offset=0");

  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(stringify(expected), response);

  // Percent encoded '/' urls.
  response = process::http::get(pid, "read.json?path=%2Fone%2F2%2Fthree&offset=0");

  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(stringify(expected), response);

  response = process::http::get(pid,
      "read.json?path=one%2Ftwo%2Fthree&offset=0");

  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(stringify(expected), response);

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

  // Breaking out of sandbox.
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      BadRequest().status,
      process::http::get(pid, "read.json?path=two/../two"));
}


TEST_WITH_WORKDIR(FilesTest, BrowseTest)
{
  Files files;
  const process::PID<>& pid = files.pid();

  ASSERT_TRUE(os::mkdir("1/2").isSome());
  ASSERT_TRUE(os::mkdir("1/3").isSome());
  ASSERT_TRUE(os::write("1/two", "two").isSome());
  ASSERT_TRUE(os::write("1/three", "three").isSome());

  EXPECT_FUTURE_WILL_SUCCEED(files.attach("1", "one"));

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

  Future<Response> response = process::http::get(pid, "browse.json?path=one/");

  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(stringify(expected), response);

  response = process::http::get(pid, "browse.json?path=one%2F");

  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(stringify(expected), response);

  response = process::http::get(pid, "browse.json?path=one");

  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(stringify(expected), response);

  // Empty listing.
  response = process::http::get(pid, "browse.json?path=one/2");

  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_BODY_WILL_EQ(stringify(JSON::Array()), response);

  // Missing dir.
  EXPECT_RESPONSE_STATUS_WILL_EQ(
      NotFound().status,
      process::http::get(pid, "browse.json?path=missing"));
}


TEST_WITH_WORKDIR(FilesTest, DownloadTest)
{
  Files files;
  const process::PID<>& pid = files.pid();

  // This is a one-pixel black gif image.
  char gifData[] = {
      0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x01, 0x00, 0x01, 0x00, 0x91, 0x00,
      0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02,
      0x02, 0x4c, 0x01, 0x00, 0x3b, 0x00
  };
  string data(gifData, sizeof(gifData));

  ASSERT_TRUE(os::write("binary", "no file extension").isSome());
  ASSERT_TRUE(os::write("black.gif", data).isSome());
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("binary", "binary"));
  EXPECT_FUTURE_WILL_SUCCEED(files.attach("black.gif", "black.gif"));

  Future<Response> response =
    process::http::get(pid, "download.json?path=binary");
  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_HEADER_WILL_EQ(
      "application/octet-stream",
      "Content-Type",
      response);
  EXPECT_RESPONSE_BODY_WILL_EQ("no file extension", response);

  response = process::http::get(pid, "download.json?path=black.gif");
  EXPECT_RESPONSE_STATUS_WILL_EQ(OK().status, response);
  EXPECT_RESPONSE_HEADER_WILL_EQ("image/gif", "Content-Type", response);
  EXPECT_RESPONSE_BODY_WILL_EQ(data, response);
}
