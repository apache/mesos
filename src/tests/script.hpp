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

#ifndef __TESTS_SCRIPT_HPP__
#define __TESTS_SCRIPT_HPP__

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <stout/preprocessor.hpp>

namespace mesos {
namespace internal {
namespace tests {

// Helper used by TEST_SCRIPT to execute the script.
void execute(
    const std::string& script,
    const std::vector<std::string>& arguments = std::vector<std::string>());

} // namespace tests {
} // namespace internal {
} // namespace mesos {


// Runs the given script (relative to src/tests). We execute this
// script in temporary directory and pipe its output to '/dev/null'
// unless the verbose option is specified. The "test" passes if the
// script returns 0.
#define TEST_SCRIPT_0(test_case_name, test_name, script)               \
  TEST(test_case_name, test_name) {                                    \
    mesos::internal::tests::execute(script);                           \
  }

#define TEST_SCRIPT_1(test_case_name, test_name, script, arg1)         \
  TEST(test_case_name, test_name) {                                    \
    std::vector<std::string> arguments;                                \
    arguments.push_back(arg1);                                         \
    mesos::internal::tests::execute(script, arguments);                \
  }

#define TEST_SCRIPT_2(test_case_name, test_name, script, arg1, arg2)   \
  TEST(test_case_name, test_name) {                                    \
    std::vector<std::string> arguments;                                \
    arguments.push_back(arg1);                                         \
    arguments.push_back(arg2);                                         \
    mesos::internal::tests::execute(script, arguments);                \
  }

#define TEST_SCRIPT_3(test_case_name, test_name, script, arg1, arg2, arg3)   \
  TEST(test_case_name, test_name) {                                          \
    std::vector<std::string> arguments;                                      \
    arguments.push_back(arg1);                                               \
    arguments.push_back(arg2);                                               \
    arguments.push_back(arg3);                                               \
    mesos::internal::tests::execute(script, arguments);                      \
  }

#endif // __TESTS_SCRIPT_HPP__
