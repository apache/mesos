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
  const vector<string> inputs = {
    "0.0.0",
    "0.2.3",
    "0.9.9",
    "0.10.4",
    "0.20.3",
    "1.0.0-alpha",
    "1.0.0-alpha.1",
    "1.0.0-alpha.-02",
    "1.0.0-alpha.-1",
    "1.0.0-alpha.1-1",
    "1.0.0-alpha.beta",
    "1.0.0-beta",
    "1.0.0-beta.2",
    "1.0.0-beta.11",
    "1.0.0-rc.1",
    "1.0.0-rc.1.2",
    "1.0.0",
    "1.0.1",
    "2.0.0"
  };

  vector<Version> versions;

  foreach (const string& input, inputs) {
    Try<Version> version = Version::parse(input);
    ASSERT_SOME(version);

    versions.push_back(version.get());
  }

  // Check that `versions` is in ascending order.
  for (size_t i = 0; i < versions.size(); i++) {
    EXPECT_FALSE(versions[i] < versions[i])
      << "Expected " << versions[i] << " < " << versions[i] << " to be false";

    for (size_t j = i + 1; j < versions.size(); j++) {
      EXPECT_TRUE(versions[i] < versions[j])
        << "Expected " << versions[i] << " < " << versions[j];

      EXPECT_FALSE(versions[j] < versions[i])
        << "Expected " << versions[i] << " < " << versions[j] << " to be false";
    }
  }
}


// Verify that build metadata labels are ignored when determining
// equality and ordering between versions.
TEST(VersionTest, BuildMetadataComparison)
{
  Version plain = Version(1, 2, 3);
  Version buildMetadata = Version(1, 2, 3, {}, {"abc"});

  EXPECT_TRUE(plain == buildMetadata);
  EXPECT_FALSE(plain != buildMetadata);
  EXPECT_FALSE(plain < buildMetadata);
  EXPECT_FALSE(plain > buildMetadata);
}


// Verify that valid version strings are parsed successfully.
TEST(VersionTest, ParseValid)
{
  // Each test case consists of an input value and a corresponding
  // expected value: the `Version` that corresponds to the input, and
  // the result of `operator<<` for that Version.

  typedef pair<Version, string> ExpectedValue;

  const map<string, ExpectedValue> testCases = {
    {"1.20.3", {Version(1, 20, 3), "1.20.3"}},
    {"1.20", {Version(1, 20, 0), "1.20.0"}},
    {"1", {Version(1, 0, 0), "1.0.0"}},
    {"1.20.3-rc1", {Version(1, 20, 3, {"rc1"}), "1.20.3-rc1"}},
    {"1.20.3--", {Version(1, 20, 3, {"-"}), "1.20.3--"}},
    {"1.20.3+-.-", {Version(1, 20, 3, {}, {"-", "-"}), "1.20.3+-.-"}},
    {"1.0.0-alpha.1", {Version(1, 0, 0, {"alpha", "1"}), "1.0.0-alpha.1"}},
    {"1.0.0-alpha+001",
     {Version(1, 0, 0, {"alpha"}, {"001"}), "1.0.0-alpha+001"}},
    {"1.0.0-alpha.-123",
     {Version(1, 0, 0, {"alpha", "-123"}), "1.0.0-alpha.-123"}},
    {"1+20130313144700",
     {Version(1, 0, 0, {}, {"20130313144700"}), "1.0.0+20130313144700"}},
    {"1.0.0-beta+exp.sha.5114f8",
     {Version(1, 0, 0, {"beta"}, {"exp", "sha", "5114f8"}),
      "1.0.0-beta+exp.sha.5114f8"}},
    {"1.0.0--1", {Version(1, 0, 0, {"-1"}), "1.0.0--1"}},
    {"1.0.0-----1", {Version(1, 0, 0, {"----1"}), "1.0.0-----1"}},
    {"1-2-3+4-5",
     {Version(1, 0, 0, {"2-3"}, {"4-5"}), "1.0.0-2-3+4-5"}},
    {"1-2-3.4+5.6-7",
     {Version(1, 0, 0, {"2-3", "4"}, {"5", "6-7"}), "1.0.0-2-3.4+5.6-7"}},
    {"1-2.-3+4.-5",
     {Version(1, 0, 0, {"2", "-3"}, {"4", "-5"}), "1.0.0-2.-3+4.-5"}},
    // Allow leading zeros: in violation of the SemVer spec, but we
    // tolerate it for compatibility with common practice (e.g., Docker).
    {"01.2.3", {Version(1, 2, 3), "1.2.3"}},
    {"1.02.3", {Version(1, 2, 3), "1.2.3"}},
    {"1.2.03", {Version(1, 2, 3), "1.2.3"}},
    {"1.2.3-alpha.001", {Version(1, 2, 3, {"alpha", "001"}), "1.2.3-alpha.001"}}
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
    "0.1.-2",
    "0.-1.2",
    "1.2.3.4",
    "-1.1.2",
    "1.1.2-",
    "1.1.2+",
    "1.1.2-+",
    "1.1.2-.",
    "1.1.2+.",
    "1.1.2-foo..",
    "1.1.2-.foo",
    "1.1.2+",
    "1.1.2+foo..",
    "1.1.2+.foo",
    "1.1.2-al^pha",
    "1.1.2+exp;",
    "-foo",
    "+foo",
    u8"1.0.0-b\u00e9ta"
  };

  foreach (const string& input, inputs) {
    Try<Version> parse = Version::parse(input);

    EXPECT_ERROR(parse)
      << "Expected error on input '" << input
      << "'; got " << parse.get();
  }
}
