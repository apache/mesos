/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/stringify.hpp>
#include <stout/version.hpp>


// Verify version comparison operations.
TEST(VersionTest, Comparison)
{
  Version version1(0, 10, 4);
  Version version2(0, 20, 3);
  Try<Version> version3 = Version::parse("0.20.3");

  EXPECT_EQ(version2, version3.get());
  EXPECT_NE(version1, version2);
  EXPECT_LT(version1, version2);
  EXPECT_LE(version1, version2);
  EXPECT_LE(version2, version3.get());
  EXPECT_GT(version2, version1);
  EXPECT_GE(version2, version1);
  EXPECT_GE(version3.get(), version1);

  EXPECT_EQ(stringify(version2), "0.20.3");
}


// Verify version parser.
TEST(VersionTest, Parse)
{
  Try<Version> version1 = Version::parse("1.20.3");
  Try<Version> version2 = Version::parse("1.20");
  Try<Version> version3 = Version::parse("1");

  EXPECT_GT(version1.get(), version2.get());
  EXPECT_GT(version2.get(), version3.get());
  EXPECT_EQ(stringify(version2.get()), "1.20.0");
  EXPECT_EQ(stringify(version3.get()), "1.0.0");

  // Verify that tagged/labeled versions work.
  Try<Version> version4 = Version::parse("1.20.3-rc1");
  EXPECT_SOME(version4);
  EXPECT_EQ(version4.get(), version1.get());
  EXPECT_EQ(stringify(version4.get()), "1.20.3");

  EXPECT_ERROR(Version::parse("0.a.b"));
  EXPECT_ERROR(Version::parse(""));
  EXPECT_ERROR(Version::parse("a"));
  EXPECT_ERROR(Version::parse("1."));
  EXPECT_ERROR(Version::parse(".1.2"));
  EXPECT_ERROR(Version::parse("0.1.2.3"));
}
