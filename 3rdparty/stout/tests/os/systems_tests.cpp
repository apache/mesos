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

#include <stout/os.hpp>

#include <stout/tests/utils.hpp>

using std::string;


class SystemsTests : public TemporaryDirectoryTest {};


TEST_F(SystemsTests, Uname)
{
  const Try<os::UTSInfo> info = os::uname();

  ASSERT_SOME(info);
#ifdef __linux__
  EXPECT_EQ(info->sysname, "Linux");

  // Machine arch must be non-empty.
  EXPECT_FALSE(info->machine.empty());
#elif defined(__APPLE__)
  EXPECT_EQ(info->sysname, "Darwin");

  // Machine arch must be non-empty.
  EXPECT_FALSE(info->machine.empty());
#endif // __linux__

  // The `release`, `version`, and `nodename` properties should all be
  // populated with a string of at least 1 character.
  EXPECT_GT(info->release.size(), 0u);
  EXPECT_GT(info->version.size(), 0u);
  EXPECT_GT(info->nodename.size(), 0u);
}


TEST_F(SystemsTests, Sysname)
{
  const Try<string> name = os::sysname();

  ASSERT_SOME(name);
#ifdef __linux__
  EXPECT_EQ(name.get(), "Linux");
#elif defined(__APPLE__)
  EXPECT_EQ(name.get(), "Darwin");
#endif // __linux__
}


TEST_F(SystemsTests, Release)
{
  const Try<Version> info = os::release();

  ASSERT_SOME(info);
}
