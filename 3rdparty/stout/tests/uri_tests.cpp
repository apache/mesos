// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <stout/uri.hpp>


TEST(UriTest, TestUriFromPath)
{
  EXPECT_EQ(uri::FILE_PREFIX, uri::from_path(""));

  EXPECT_EQ(
      uri::FILE_PREFIX + "/absolute/path/on/linux",
      uri::from_path("/absolute/path/on/linux"));

#ifdef __WINDOWS__
  EXPECT_EQ(
      uri::FILE_PREFIX + "C:/somedir/somefile",
      uri::from_path("C:\\somedir\\somefile"));
#endif // __WINDOWS__
}
