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

#include <string>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/stringify.hpp>
#include <stout/variant.hpp>


TEST(VariantTest, Visit)
{
  Variant<int, std::string> v1 = "hello world";

  EXPECT_TRUE(
      v1.visit([](int) { return false; },
               [](const std::string&) { return true; }));

  EXPECT_TRUE(
      v1.visit([](int) { return false; },
               [](std::string&) { return true; }));

  EXPECT_TRUE(
      v1.visit([](int) { return false; },
               [](std::string) { return true; }));

  EXPECT_EQ(
      "hello world",
      v1.visit([](int i) { return stringify(i); },
               [](const std::string& s) { return s; }));

  // NOTE: we're explicitly using `const` here as it tests both that
  // our generic constructor properly works (see comments in
  // variant.hpp) as well as that visitation works when the type is
  // `const`.
  const Variant<int, std::string> v2 = 42;

  EXPECT_TRUE(
      v2.visit([](int) { return true; },
               [](const std::string&) { return false; }));

  EXPECT_TRUE(
      v2.visit([](int) { return true; },
               [](std::string) { return false; }));

  EXPECT_EQ(
      "42",
      v2.visit([](int i) { return stringify(i); },
               [](const std::string& s) { return s; }));
}


TEST(VariantTest, Equality)
{
  Variant<int, std::string> v1 = "hello world";
  Variant<int, std::string> v2 = "hello world";
  EXPECT_EQ(v1, v2);

  Variant<int, std::string> v3 = 42;
  EXPECT_NE(v1, v3);
  EXPECT_NE(v2, v3);

  Variant<int, std::string> v4 = 42;
  EXPECT_NE(v1, v4);
  EXPECT_NE(v2, v4);
  EXPECT_EQ(v3, v4);
}
