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

#include <gtest/gtest.h>

#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include <process/owned.hpp>

#include "tests/mesos.hpp"

#include "tests/containerizer/docker_archive.hpp"

using std::string;

using process::Future;
using process::Owned;

namespace mesos {
namespace internal {
namespace tests {

#ifdef __linux__
class DockerArchiveTest : public TemporaryDirectoryTest {};


// This test verifies that a testing docker image is created as
// a local archive under a given directory. This is a ROOT test
// because permission is need to create linux root filesystem.
TEST_F(DockerArchiveTest, ROOT_CreateDockerLocalTar)
{
  const string directory = path::join(os::getcwd(), "archives");

  Future<Nothing> testImage = DockerArchive::create(directory, "alpine");
  AWAIT_READY(testImage);

  EXPECT_TRUE(os::exists(path::join(directory, "alpine.tar")));
  EXPECT_FALSE(os::exists(path::join(directory, "alpine")));
}

#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
