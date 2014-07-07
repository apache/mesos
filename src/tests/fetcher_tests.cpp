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

#include <unistd.h>

#include <gmock/gmock.h>

#include <string>

#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/subprocess.hpp>

#include <stout/gtest.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "tests/environment.hpp"
#include "tests/flags.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using process::Subprocess;
using process::Future;

using std::string;
using std::map;

class FetcherTest : public TemporaryDirectoryTest {};


TEST_F(FetcherTest, FileURI)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_FALSE(os::write(testFile, "data").isError());

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  map<string, string> env;

  env["MESOS_EXECUTOR_URIS"] = "file://" + testFile + "+0N";
  env["MESOS_WORK_DIRECTORY"] = os::getcwd();

  Try<Subprocess> fetcherProcess =
    process::subprocess(
      path::join(mesos::internal::tests::flags.build_dir, "src/mesos-fetcher"),
      env);

  ASSERT_SOME(fetcherProcess);
  Future<Option<int> > status = fetcherProcess.get().status();

  AWAIT_READY(status);
  ASSERT_SOME(status.get());

  EXPECT_EQ(0, status.get().get());
  EXPECT_TRUE(os::exists(localFile));
}


TEST_F(FetcherTest, FileLocalhostURI)
{
  string fromDir = path::join(os::getcwd(), "from");
  ASSERT_SOME(os::mkdir(fromDir));
  string testFile = path::join(fromDir, "test");
  EXPECT_FALSE(os::write(testFile, "data").isError());

  string localFile = path::join(os::getcwd(), "test");
  EXPECT_FALSE(os::exists(localFile));

  map<string, string> env;

  env["MESOS_EXECUTOR_URIS"] = path::join("file://localhost", testFile) + "+0N";
  env["MESOS_WORK_DIRECTORY"] = os::getcwd();

  Try<Subprocess> fetcherProcess =
    process::subprocess(
      path::join(mesos::internal::tests::flags.build_dir, "src/mesos-fetcher"),
      env);

  ASSERT_SOME(fetcherProcess);
  Future<Option<int> > status = fetcherProcess.get().status();

  AWAIT_READY(status);
  ASSERT_SOME(status.get());

  EXPECT_EQ(0, status.get().get());
  EXPECT_TRUE(os::exists(localFile));
}
