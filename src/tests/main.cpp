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

#include <stdlib.h>

#include <string>

#include <process/process.hpp>

#include "common/fatal.hpp"

#include "configurator/configurator.hpp"

#include "tests/utils.hpp"

using namespace mesos::internal;
using namespace mesos::internal::test;

using std::string;

namespace {

// TODO(John Sirois): Consider lifting this to common/utils.
string getRealpath(const string& relPath)
{
  char path[PATH_MAX];
  if (realpath(relPath.c_str(), path) == 0) {
    fatalerror(
        string("Failed to find location of " + relPath + " using realpath")
            .c_str());
  }
  return path;
}


string getMesosRoot()
{
  return getRealpath(ROOT_DIR);
}


string getMesosHome()
{
  return getRealpath(BUILD_DIR);
}

}

int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // Get the absolute path to the Mesos project root directory.
  mesos::internal::test::mesosRoot = getMesosRoot();

  std::cout << "MESOS_ROOT: " << mesos::internal::test::mesosRoot << std::endl;

  // Get absolute path to Mesos home install directory.
  mesos::internal::test::mesosHome = getMesosHome();

  std::cout << "MESOS_HOME: " << mesos::internal::test::mesosHome << std::endl;

  // Clear any MESOS_ environment variables so they don't affect our tests.
  Configurator::clearMesosEnvironmentVars();

  // Initialize Google Logging and Google Test.
  google::InitGoogleLogging("alltests");
  testing::InitGoogleTest(&argc, argv);
  testing::FLAGS_gtest_death_test_style = "threadsafe";
  if (argc == 2 && strcmp("-v", argv[1]) == 0)
    google::SetStderrLogging(google::INFO);

  // Initialize libprocess library (but not glog, done above).
  process::initialize(false);

  return RUN_ALL_TESTS();
}
