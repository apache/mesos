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

#include <glog/logging.h>

#include <gtest/gtest.h>

#include <string>

#include <process/process.hpp>

#include "common/utils.hpp"

#include "configurator/configurator.hpp"

#include "tests/utils.hpp"

using namespace mesos::internal;
using namespace mesos::internal::test;

using std::string;

int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // Initialize glog.
  google::InitGoogleLogging("alltests");

  if (argc == 2 && strcmp("-v", argv[1]) == 0) {
    google::SetStderrLogging(google::INFO);
  }

  // Initialize gmock/gtest.
  testing::InitGoogleTest(&argc, argv);
  testing::FLAGS_gtest_death_test_style = "threadsafe";

  // Initialize libprocess (but not glog, done above).
  process::initialize(false);

  // Get the absolute path to the source (i.e., root) directory.
  Try<string> path = utils::os::realpath(SOURCE_DIR);
  CHECK(path.isSome()) << "Error getting source directory " << path.error();
  mesosSourceDirectory = path.get();

  std::cout << "Source directory: " << mesosSourceDirectory << std::endl;

  // Get absolute path to the build directory.
  path = utils::os::realpath(BUILD_DIR);
  CHECK(path.isSome()) << "Error getting build directory " << path.error();
  mesosBuildDirectory = path.get();

  std::cout << "Build directory: " << mesosBuildDirectory << std::endl;

  // Clear any MESOS_ environment variables so they don't affect our tests.
  Configurator::clearMesosEnvironmentVars();

  return RUN_ALL_TESTS();
}
