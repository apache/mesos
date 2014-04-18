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

} // namespace tests {
} // namespace internal {
} // namespace mesos {
