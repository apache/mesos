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

#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/utils.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace tests {

void TemporaryDirectoryTest::SetUp()
{
  // Save the current working directory.
  cwd = os::getcwd();

  // Create a temporary directory for the test.
  Try<string> directory = environment->mkdtemp();

  ASSERT_SOME(directory) << "Failed to mkdtemp";

  sandbox = directory.get();

  if (flags.verbose) {
    std::cout << "Using temporary directory '"
              << sandbox.get() << "'" << std::endl;
  }

  // Run the test out of the temporary directory we created.
  ASSERT_SOME(os::chdir(sandbox.get()))
    << "Failed to chdir into '" << sandbox.get() << "'";
}


void TemporaryDirectoryTest::TearDown()
{
  // Return to previous working directory and cleanup the sandbox.
  ASSERT_SOME(os::chdir(cwd));

  if (sandbox.isSome()) {
    ASSERT_SOME(os::rmdir(sandbox.get()));
  }
}


JSON::Object Metrics()
{
  process::UPID upid("metrics", process::address());

  process::Future<process::http::Response> response =
      process::http::get(upid, "snapshot");
  AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);

  EXPECT_SOME_EQ(
      "application/json",
      response.get().headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.get().body);
  CHECK_SOME(parse);

  return parse.get();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
