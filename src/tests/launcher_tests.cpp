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

#include <stdio.h>

#include <gtest/gtest.h>

#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include "launcher/launcher.hpp"

#include "tests/flags.hpp"
#include "tests/utils.hpp"

using namespace process;

using namespace mesos::internal;
using namespace mesos::internal::launcher;

using std::string;


class LauncherTest: public tests::TemporaryDirectoryTest {};


TEST_F(LauncherTest, Launch)
{
  Option<int> stdout = None();
  Option<int> stderr = None();

  // Redirect output if running the tests verbosely.
  if (tests::flags.verbose) {
    stdout = STDOUT_FILENO;
    stderr = STDERR_FILENO;
  }

  string temp1 = path::join(os::getcwd(), "temp1");
  string temp2 = path::join(os::getcwd(), "temp2");

  ASSERT_SOME(os::write(temp1, "hello world"));

  ShellOperation operation;
  operation.flags.command = "cp " + temp1 + " " + temp2;

  Future<Option<int> > launch = operation.launch(stdout, stderr);
  AWAIT_READY(launch);
  EXPECT_SOME_EQ(0, launch.get());
  ASSERT_SOME_EQ("hello world", os::read(temp2));

  AWAIT_FAILED(operation.launch(
      stdout,
      stderr,
      "non-exist"));

  AWAIT_FAILED(operation.launch(
      stdout,
      stderr,
      launcher::DEFAULT_EXECUTABLE,
      "non-exist"));
}
