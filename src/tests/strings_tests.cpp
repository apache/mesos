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

#include <gtest/gtest.h>

#include "common/strings.hpp"

using std::map;
using std::string;
using std::vector;


TEST(StringsTest, Format)
{
  Try<std::string> result = strings::format("%s %s", "hello", "world");
  ASSERT_TRUE(result.isSome());
  EXPECT_EQ("hello world", result.get());

  result = strings::format("hello %d", 42);
  ASSERT_TRUE(result.isSome());
  EXPECT_EQ("hello 42", result.get());

  result = strings::format("hello %s", "fourty-two");
  ASSERT_TRUE(result.isSome());
  EXPECT_EQ("hello fourty-two", result.get());
}


TEST(StringsTest, Remove)
{
  EXPECT_EQ("heo word", strings::remove("hello world", "l"));
  EXPECT_EQ("hel world", strings::remove("hello world", "lo"));
  EXPECT_EQ("home/", strings::remove("/home/", "/", strings::PREFIX));
  EXPECT_EQ("/home", strings::remove("/home/", "/", strings::SUFFIX));
}


TEST(StringsTest, Trim)
{
  EXPECT_EQ("", strings::trim("", " "));
  EXPECT_EQ("", strings::trim("    ", " "));
  EXPECT_EQ("hello world", strings::trim("hello world", " "));
  EXPECT_EQ("hello world", strings::trim("  hello world", " "));
  EXPECT_EQ("hello world", strings::trim("hello world  ", " "));
  EXPECT_EQ("hello world", strings::trim("  hello world  ", " "));
  EXPECT_EQ("hello world", strings::trim(" \t hello world\t  ", " \t"));
  EXPECT_EQ("hello world", strings::trim(" \t hello world\t \n\r "));
}


TEST(StringsTest, SplitEmptyString)
{
  vector<string> tokens = strings::split("", " ");
  ASSERT_EQ(0, tokens.size());
}


TEST(StringsTest, SplitDelimOnlyString)
{
  vector<string> tokens = strings::split("   ", " ");
  ASSERT_EQ(0, tokens.size());
}


TEST(StringsTest, Split)
{
  vector<string> tokens = strings::split("hello world,  what's up?", " ");
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringsTest, SplitStringWithDelimsAtStart)
{
  vector<string> tokens = strings::split("  hello world,  what's up?", " ");
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringsTest, SplitStringWithDelimsAtEnd)
{
  vector<string> tokens = strings::split("hello world,  what's up?  ", " ");
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringsTest, SplitStringWithDelimsAtStartAndEnd)
{
  vector<string> tokens = strings::split("  hello world,  what's up?  ", " ");
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringsTest, SplitWithMultipleDelims)
{
  vector<string> tokens = strings::split("hello\tworld,  \twhat's up?", " \t");
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringsTest, Pairs)
{
  map<string, vector<string> > pairs = strings::pairs("one=1,two=2", ',', '=');
  ASSERT_EQ(2, pairs.size());
  ASSERT_EQ(1, pairs.count("one"));
  ASSERT_EQ(1, pairs["one"].size());
  EXPECT_EQ("1", pairs["one"].front());
  ASSERT_EQ(1, pairs.count("two"));
  ASSERT_EQ(1, pairs["two"].size());
  EXPECT_EQ("2", pairs["two"].front());
}
