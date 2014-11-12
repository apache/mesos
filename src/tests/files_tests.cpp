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
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>

#include "files/files.hpp"

#include "tests/utils.hpp"

using namespace mesos::internal;
using namespace mesos::internal::tests;

using process::Future;

using process::http::BadRequest;
using process::http::NotFound;
using process::http::OK;
using process::http::Response;

using std::string;


class FilesTest : public TemporaryDirectoryTest {};


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
  process::UPID upid("files", process::node());

  Future<Response> response =
    process::http::get(upid, "read.json");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Expecting 'path=value' in query.\n",
      response);

  response = process::http::get(upid, "read.json", "path=none&offset=hello");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(BadRequest().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(
      "Failed to parse offset: Failed to convert 'hello' to number.\n",
      response);

  response = process::http::get(upid, "read.json", "path=none&length=hello");

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

  response = process::http::get(upid, "read.json", "path=/myname&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response = process::http::get(upid, "read.json", "path=myname&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  // Missing file.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      NotFound().status,
      process::http::get(upid, "read.json", "path=missing"));
}


TEST_F(FilesTest, ResolveTest)
{
  Files files;
  process::UPID upid("files", process::node());

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
    process::http::get(upid, "read.json", "path=one/2/three&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response =
    process::http::get(upid, "read.json", "path=/one/2/three&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response =
    process::http::get(upid, "read.json", "path=two/three&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response =
    process::http::get(upid, "read.json", "path=one/two/three&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  // Percent encoded '/' urls.
  response =
    process::http::get(upid, "read.json", "path=%2Fone%2F2%2Fthree&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response =
    process::http::get(upid, "read.json", "path=one%2Ftwo%2Fthree&offset=0");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  // Reading dirs not allowed.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      BadRequest().status,
      process::http::get(upid, "read.json", "path=one/2"));
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      BadRequest().status,
      process::http::get(upid, "read.json", "path=one"));
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      BadRequest().status,
      process::http::get(upid, "read.json", "path=one/"));

  // Breaking out of sandbox.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      BadRequest().status,
      process::http::get(upid, "read.json", "path=two/../two"));
}


TEST_F(FilesTest, BrowseTest)
{
  Files files;
  process::UPID upid("files", process::node());

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
      process::http::get(upid, "browse.json", "path=one/");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response = process::http::get(upid, "browse.json", "path=one%2F");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  response = process::http::get(upid, "browse.json", "path=one");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(expected), response);

  // Empty listing.
  response = process::http::get(upid, "browse.json", "path=one/2");

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(stringify(JSON::Array()), response);

  // Missing dir.
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      NotFound().status,
      process::http::get(upid, "browse.json", "path=missing"));
}


TEST_F(FilesTest, DownloadTest)
{
  Files files;
  process::UPID upid("files", process::node());

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
    process::http::get(upid, "download.json", "path=binary");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(
      "application/octet-stream",
      "Content-Type",
      response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ("no file extension", response);

  response = process::http::get(upid, "download.json", "path=black.gif");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ("image/gif", "Content-Type", response);
  AWAIT_EXPECT_RESPONSE_BODY_EQ(data, response);
}
