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

#ifndef __EXTERNAL_TEST_HPP__
#define __EXTERNAL_TEST_HPP__

#include <gtest/gtest.h>

/**
 * Run an external test with the given name. The test is expected to be
 * located in src/tests/external/<testCase>/<testName>.sh.
 * We execute this script in directory test_output/<testCase>/<testName>,
 * piping its output to files called stdout and stderr, and the test
 * passes if the script returns 0.
 */
#define TEST_EXTERNAL(testCase, testName) \
  TEST(testCase, testName) { \
    mesos::internal::test::runExternalTest(#testCase, #testName); \
  }

namespace mesos { namespace internal { namespace test {

/**
 * Function called by TEST_EXTERNAL to execute external tests. See
 * explanation of parameters at definition of TEST_EXTERNAL.
 */
void runExternalTest(const char* testCase, const char* testName);

}}} /* namespace mesos::internal::test */

#endif /* __EXTERNAL_TEST_HPP__ */
