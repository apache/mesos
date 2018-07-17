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

#ifndef __TESTS_ENVIRONMENT_HPP__
#define __TESTS_ENVIRONMENT_HPP__

#include <list>
#include <string>

#include <gtest/gtest.h>

#include <stout/try.hpp>

#include <stout/tests/environment.hpp>

#include "tests/flags.hpp"

namespace mesos {
namespace internal {
namespace tests {

// Used to set up and manage the test environment.
class Environment : public stout::internal::tests::Environment {
public:
  Environment(const Flags& flags);

  void SetUp() override;

  void TearDown() override;

  // Helper to create a temporary directory based on the current test
  // case name and test name (derived from TestInfo via
  // ::testing::UnitTest::GetInstance()->current_test_info()). Note
  // that the directory will automagically get removed when the
  // test finishes.
  Try<std::string> mkdtemp();

private:
  // Used to clean up temporary directories after tests finish.
  class TemporaryDirectoryEventListener
    : public ::testing::EmptyTestEventListener
  {
  public:
    Try<std::string> mkdtemp();

  protected:
    void OnTestEnd(const ::testing::TestInfo&) override;

    // Temporary directories that we created and need to remove.
    std::list<std::string> directories;
  };

  const Flags flags;

  TemporaryDirectoryEventListener* temporaryDirectoryEventListener;
};


// Global environment instance.
extern Environment* environment;

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_ENVIRONMENT_HPP__
