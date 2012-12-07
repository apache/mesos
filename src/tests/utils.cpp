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

#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include "tests/flags.hpp"
#include "tests/utils.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace tests {

// Storage for our flags.
flags::Flags<logging::Flags, Flags> flags;


Try<string> mkdtemp()
{
  const ::testing::TestInfo* const testInfo =
    ::testing::UnitTest::GetInstance()->current_test_info();

  const char* testCase = testInfo->test_case_name();
  const char* testName = testInfo->name();

  // Adjust the test name to remove any 'DISABLED_' prefix (to make
  // things easier to read). While this might seem alarming, if we are
  // "running" a disabled test it must be the case that the test was
  // explicitly enabled (e.g., via 'gtest_filter').
  if (strings::startsWith(testName, "DISABLED_")) {
    testName += strlen("DISABLED_");
  }

  const string& path =
    path::join("/tmp", strings::join("_", testCase, testName, "XXXXXX"));

  return os::mkdtemp(path);
}


void TemporaryDirectoryTest::SetUp()
{
  // Save the current working directory.
  cwd = os::getcwd();

  // Create a temporary directory for the test.
  Try<string> directory = mkdtemp();

  CHECK_SOME(directory) << "Failed to create mkdtemp";

  if (flags.verbose) {
    std::cerr << "Using temporary directory '"
              << directory.get() << "'" << std::endl;
  }

  // Run the test out of the temporary directory we created.
  PCHECK(os::chdir(directory.get()))
    << "Failed to chdir into '" << directory.get() << "'";
}


void TemporaryDirectoryTest::TearDown()
{
  // Return to previous working directory.
  PCHECK(os::chdir(cwd));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
