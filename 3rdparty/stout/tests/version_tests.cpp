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

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/stringify.hpp>
#include <stout/version.hpp>

using std::map;
using std::pair;
using std::string;
using std::vector;

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


// Verify that valid version strings are parsed successfully.
TEST(VersionTest, ParseValid)
{
  // Each test case consists of an input value and a corresponding
  // expected value: the `Version` that corresponds to the input, and
  // the result of `operator<<` for that Version.

  typedef pair<Version, string> ExpectedValue;

  const map<string, ExpectedValue> testCases = {
    // Prerelease labels are currently accepted but ignored.
    {"1.20.3-rc1", {Version(1, 20, 3), "1.20.3"}},
    {"1.20.3", {Version(1, 20, 3), "1.20.3"}},
    {"1.20", {Version(1, 20, 0), "1.20.0"}},
    {"1", {Version(1, 0, 0), "1.0.0"}}
  };

  foreachpair (const string& input, const ExpectedValue& expected, testCases) {
    Try<Version> actual = Version::parse(input);

    ASSERT_SOME(actual)
      << "Error parsing input '" << input << "'";

    EXPECT_EQ(std::get<0>(expected), actual.get())
      << "Incorrect parse of input '" << input << "'";
    EXPECT_EQ(std::get<1>(expected), stringify(actual.get()))
      << "Unexpected stringify output for input '" << input << "'";
  }
}


// Verify that invalid version strings result in a parse error.
TEST(VersionTest, ParseInvalid)
{
  const vector<string> inputs = {
    "",
    "0.a.b",
    "a",
    "1.",
    ".1.2",
    "0.1.2.3",
    "-1.1.2"
  };

  foreach (const string& input, inputs) {
    Try<Version> parse = Version::parse(input);

    EXPECT_ERROR(parse)
      << "Expected error on input '" << input
      << "'; got " << parse.get();
  }
}
