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

#include "tests/utils.hpp"

using namespace mesos::internal;

using std::string;


string test::mesosRoot;
string test::mesosHome;


namespace {

// Check that a test name contains only letters, numbers and underscores, to
// prevent messing with directories outside test_output in runExternalTest.
bool isValidTestName(const char* name) {
  for (const char* p = name; *p != 0; p++) {
    if (!isalnum(*p) && *p != '_') {
      return false;
    }
  }
  return true;
}

} // namespace


/**
 * Create and clean up the work directory for a given test, and cd into it,
 * given the test's test case name and test name.
 * Test directories are placed in <mesosHome>/test_output/<testCase>/<testName>.
 */
void test::enterTestDirectory(const char* testCase, const char* testName)
{
  // Remove DISABLED_ prefix from test name if this is a disabled test
  if (strncmp(testName, "DISABLED_", strlen("DISABLED_")) == 0)
    testName += strlen("DISABLED_");
  // Check that the test name is valid
  if (!isValidTestName(testCase) || !isValidTestName(testName)) {
    FAIL() << "Invalid test name for external test (name should " 
           << "only contain alphanumeric and underscore characters)";
  }
  // Make the work directory for this test
  string workDir = mesosHome + "/test_output/" + testCase + "/" + testName;
  string command = "rm -fr '" + workDir + "'";
  ASSERT_EQ(0, system(command.c_str())) << "Command failed: " << command;
  command = "mkdir -p '" + workDir + "'";
  ASSERT_EQ(0, system(command.c_str())) << "Command failed: " << command;
  // Change dir into it
  if (chdir(workDir.c_str()) != 0)
    FAIL() << "Could not chdir into " << workDir;
}
