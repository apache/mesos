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

#include <string>

#include <stout/gtest.hpp>
#include <stout/path.hpp>

#include <stout/os/close.hpp>
#include <stout/os/read.hpp>
#include <stout/os/write.hpp>

#include <stout/tests/utils.hpp>

#include "linux/memfd.hpp"

using std::string;

namespace mesos {
namespace internal {
namespace tests {

class MemfdTest : public TemporaryDirectoryTest {};


TEST_F(MemfdTest, CloneSealedFile)
{
  const string content = "hello world!";
  const string filePath = path::join(sandbox.get(), "file");
  ASSERT_SOME(os::write(filePath, content));

  Try<int_fd> fd = memfd::cloneSealedFile(filePath);
  ASSERT_SOME(fd);

  const string memFdFile = "/proc/self/fd/" + stringify(fd.get());

  Try<string> actual = os::read(memFdFile);
  EXPECT_SOME_EQ(content, actual);

  // Make sure we cannot write the cloned file.
  EXPECT_ERROR(os::write(memFdFile, "xxx"));

  os::close(fd.get());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
