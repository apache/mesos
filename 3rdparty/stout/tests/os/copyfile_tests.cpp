// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <string>

#include <stout/os.hpp>
#include <stout/path.hpp>

#include <stout/os/copyfile.hpp>
#include <stout/os/getcwd.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/write.hpp>

#include <stout/tests/utils.hpp>

using std::string;


class CopyFileTest : public TemporaryDirectoryTest {};


TEST_F(CopyFileTest, CopyWithAbsolutePath)
{
  const string fileContents = "Some file content";

  const string sourceFile = path::join(sandbox.get(), "source-file");
  EXPECT_SOME(os::write(sourceFile, fileContents));

  const string destination = path::join(sandbox.get(), "dest-file");
  EXPECT_FALSE(os::exists(destination));

  EXPECT_SOME(os::copyfile(sourceFile, destination));
  EXPECT_TRUE(os::exists(destination));

  // Check contents of files for the correct data.
  EXPECT_SOME_EQ(fileContents, os::read(sourceFile));
  EXPECT_SOME_EQ(fileContents, os::read(destination));
}


TEST_F(CopyFileTest, CopyToDirectoryDestinationFails)
{
  const string sourceFile = path::join(sandbox.get(), "source-file");
  EXPECT_SOME(os::write(sourceFile, "Some file content"));

  const string destination = path::join(sandbox.get(), "dest-dir");
  EXPECT_FALSE(os::exists(destination));
  EXPECT_SOME(os::mkdir(destination));

  // Copying to a directory should fail.
  EXPECT_ERROR(os::copyfile(sourceFile, destination));
}


TEST_F(CopyFileTest, DestinationEndsInSlashFails)
{
  const string sourceFile = path::join(sandbox.get(), "source-file");
  EXPECT_SOME(os::write(sourceFile, "Some file content"));

  const string destination = path::join(sandbox.get(), "dest-dir/");
  EXPECT_FALSE(os::exists(destination));

  // Copying to a destination that looks like a directory should fail.
  EXPECT_ERROR(os::copyfile(sourceFile, destination));
}


TEST_F(CopyFileTest, CopyToRelativeFilenameFails)
{
  const string sourceFile = path::join(sandbox.get(), "source-file");
  EXPECT_SOME(os::write(sourceFile, "Some file content"));

  const string destination = path::join(sandbox.get(), "dest-file");

  // Relative file locations will be rejected.
  EXPECT_ERROR(os::copyfile(sourceFile, "dest-file"));
  EXPECT_ERROR(os::copyfile("source-file", destination));
  EXPECT_ERROR(os::copyfile("source-file", "dest-file"));
}
