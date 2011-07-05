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

#include <string>
#include <vector>

#include "common/string_utils.hpp"

using namespace mesos;
using namespace mesos::internal;

using std::string;
using std::vector;


TEST(StringUtilsTest, SplitEmptyString)
{
  vector<string> tokens;
  StringUtils::split("", " ", &tokens);
  ASSERT_EQ(0, tokens.size());
}


TEST(StringUtilsTest, SplitDelimOnlyString)
{
  vector<string> tokens;
  StringUtils::split("   ", " ", &tokens);
  ASSERT_EQ(0, tokens.size());
}


TEST(StringUtilsTest, Split)
{
  vector<string> tokens;
  StringUtils::split("hello world,  what's up?", " ", &tokens);
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringUtilsTest, SplitStringWithDelimsAtStart)
{
  vector<string> tokens;
  StringUtils::split("  hello world,  what's up?", " ", &tokens);
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringUtilsTest, SplitStringWithDelimsAtEnd)
{
  vector<string> tokens;
  StringUtils::split("hello world,  what's up?  ", " ", &tokens);
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringUtilsTest, SplitStringWithDelimsAtStartAndEnd)
{
  vector<string> tokens;
  StringUtils::split("  hello world,  what's up?  ", " ", &tokens);
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringUtilsTest, SplitWithMultipleDelims)
{
  vector<string> tokens;
  StringUtils::split("hello\tworld,  \twhat's up?", " \t", &tokens);
  ASSERT_EQ(4, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringUtilsTest, Trim)
{
  EXPECT_EQ("", StringUtils::trim("", " "));
  EXPECT_EQ("", StringUtils::trim("    ", " "));
  EXPECT_EQ("hello world", StringUtils::trim("hello world", " "));
  EXPECT_EQ("hello world", StringUtils::trim("  hello world", " "));
  EXPECT_EQ("hello world", StringUtils::trim("hello world  ", " "));
  EXPECT_EQ("hello world", StringUtils::trim("  hello world  ", " "));
  EXPECT_EQ("hello world", StringUtils::trim(" \t hello world\t  ", " \t"));
  EXPECT_EQ("hello world", StringUtils::trim(" \t hello world\t \n\r "));
}
