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
