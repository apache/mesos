#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <map>
#include <string>
#include <vector>

#include <stout/format.hpp>
#include <stout/gtest.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

using std::map;
using std::string;
using std::vector;


TEST(StringsTest, Format)
{
  Try<std::string> result = strings::format("%s %s", "hello", "world");
  ASSERT_SOME(result);
  EXPECT_EQ("hello world", result.get());

  result = strings::format("hello %d", 42);
  ASSERT_SOME(result);
  EXPECT_EQ("hello 42", result.get());

  result = strings::format("hello %s", "fourty-two");
  ASSERT_SOME(result);
  EXPECT_EQ("hello fourty-two", result.get());

  string hello = "hello";

  result = strings::format("%s %s", hello, "fourty-two");
  ASSERT_SOME(result);
  EXPECT_EQ("hello fourty-two", result.get());
}


TEST(StringsTest, Remove)
{
  EXPECT_EQ("heo word", strings::remove("hello world", "l"));
  EXPECT_EQ("hel world", strings::remove("hello world", "lo"));
  EXPECT_EQ("home/", strings::remove("/home/", "/", strings::PREFIX));
  EXPECT_EQ("/home", strings::remove("/home/", "/", strings::SUFFIX));
}


TEST(StringsTest, Replace)
{
  EXPECT_EQ("hello*", strings::replace("hello/", "/", "*"));
  EXPECT_EQ("*hello", strings::replace("/hello", "/", "*"));
  EXPECT_EQ("*hello*world*", strings::replace("/hello/world/", "/", "*"));
  EXPECT_EQ("*", strings::replace("/", "/", "*"));
  EXPECT_EQ("hello world", strings::replace("hello world", "/", "*"));
  EXPECT_EQ("***1***2***3***", strings::replace("/1/2/3/", "/", "***"));
  EXPECT_EQ("123", strings::replace("/1/2/3/", "/", ""));
  EXPECT_EQ("/1/2/3**", strings::replace("***1***2***3**", "***", "/"));
  EXPECT_EQ("/1/2/3/", strings::replace("/1/2/3/", "", "*"));
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


TEST(StringsTest, Tokenize)
{
  vector<string> tokens = strings::tokenize("hello world,  what's up?", " ");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringsTest, TokenizeStringWithDelimsAtStart)
{
  vector<string> tokens = strings::tokenize("  hello world,  what's up?", " ");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringsTest, TokenizeStringWithDelimsAtEnd)
{
  vector<string> tokens =
    strings::tokenize("hello world,  what's up?  ", " ");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringsTest, TokenizeStringWithDelimsAtStartAndEnd)
{
  vector<string> tokens =
    strings::tokenize("  hello world,  what's up?  ", " ");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringsTest, TokenizeWithMultipleDelims)
{
  vector<string> tokens =
    strings::tokenize("hello\tworld,  \twhat's up?", " \t");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("hello",  tokens[0]);
  EXPECT_EQ("world,", tokens[1]);
  EXPECT_EQ("what's", tokens[2]);
  EXPECT_EQ("up?",    tokens[3]);
}


TEST(StringsTest, TokenizeEmptyString)
{
  vector<string> tokens = strings::tokenize("", " ");
  ASSERT_EQ(0u, tokens.size());
}


TEST(StringsTest, TokenizeDelimOnlyString)
{
  vector<string> tokens = strings::tokenize("   ", " ");
  ASSERT_EQ(0u, tokens.size());
}


TEST(StringsTest, TokenizeNullByteDelim)
{
  string s;
  s.push_back('\0');
  s.push_back('\0');
  s.push_back('\0');
  s.push_back('h');
  s.push_back('e');
  s.push_back('l');
  s.push_back('l');
  s.push_back('o');
  s.push_back('\0');
  s.push_back('\0');
  s.push_back('\0');
  s.push_back('\0');
  s.push_back('\0');
  s.push_back('\0');
  s.push_back('w');
  s.push_back('o');
  s.push_back('r');
  s.push_back('l');
  s.push_back('d');
  s.push_back('\0');
  s.push_back('\0');
  s.push_back('\0');

  vector<string> tokens = strings::tokenize(s, string(1, '\0'));

  ASSERT_EQ(2u, tokens.size());
  EXPECT_EQ("hello", tokens[0]);
  EXPECT_EQ("world", tokens[1]);
}


TEST(StringsTest, SplitEmptyString)
{
  vector<string> tokens = strings::split("", ",");
  ASSERT_EQ(1u, tokens.size());
  EXPECT_EQ("", tokens[0]);
}


TEST(StringsTest, SplitDelimOnlyString)
{
  vector<string> tokens = strings::split(",,,", ",");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("", tokens[0]);
  EXPECT_EQ("", tokens[1]);
  EXPECT_EQ("", tokens[2]);
  EXPECT_EQ("", tokens[3]);
}


TEST(StringsTest, Split)
{
  vector<string> tokens = strings::split("foo,bar,,baz", ",");
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("foo", tokens[0]);
  EXPECT_EQ("bar", tokens[1]);
  EXPECT_EQ("",    tokens[2]);
  EXPECT_EQ("baz", tokens[3]);
}


TEST(StringsTest, SplitStringWithDelimsAtStart)
{
  vector<string> tokens = strings::split(",,foo,bar,,baz", ",");
  ASSERT_EQ(6u, tokens.size());
  EXPECT_EQ("",    tokens[0]);
  EXPECT_EQ("",    tokens[1]);
  EXPECT_EQ("foo", tokens[2]);
  EXPECT_EQ("bar", tokens[3]);
  EXPECT_EQ("",    tokens[4]);
  EXPECT_EQ("baz", tokens[5]);
}


TEST(StringsTest, SplitStringWithDelimsAtEnd)
{
  vector<string> tokens = strings::split("foo,bar,,baz,,", ",");
  ASSERT_EQ(6u, tokens.size());
  EXPECT_EQ("foo", tokens[0]);
  EXPECT_EQ("bar", tokens[1]);
  EXPECT_EQ("",    tokens[2]);
  EXPECT_EQ("baz", tokens[3]);
  EXPECT_EQ("",    tokens[4]);
  EXPECT_EQ("",    tokens[5]);
}


TEST(StringsTest, SplitStringWithDelimsAtStartAndEnd)
{
  vector<string> tokens = strings::split(",,foo,bar,,", ",");
  ASSERT_EQ(6u, tokens.size());
  EXPECT_EQ("",    tokens[0]);
  EXPECT_EQ("",    tokens[1]);
  EXPECT_EQ("foo", tokens[2]);
  EXPECT_EQ("bar", tokens[3]);
  EXPECT_EQ("",    tokens[4]);
  EXPECT_EQ("",    tokens[5]);
}


TEST(StringsTest, SplitWithMultipleDelims)
{
  vector<string> tokens = strings::split("foo.bar,.,.baz.", ",.");
  ASSERT_EQ(7u, tokens.size());
  EXPECT_EQ("foo", tokens[0]);
  EXPECT_EQ("bar", tokens[1]);
  EXPECT_EQ("",    tokens[2]);
  EXPECT_EQ("",    tokens[3]);
  EXPECT_EQ("",    tokens[4]);
  EXPECT_EQ("baz", tokens[5]);
  EXPECT_EQ("",    tokens[6]);
}


TEST(StringsTest, Pairs)
{
  map<string, vector<string> > pairs = strings::pairs("one=1,two=2", ",", "=");
  ASSERT_EQ(2u, pairs.size());
  ASSERT_EQ(1u, pairs.count("one"));
  ASSERT_EQ(1u, pairs["one"].size());
  EXPECT_EQ("1", pairs["one"].front());
  ASSERT_EQ(1u, pairs.count("two"));
  ASSERT_EQ(1u, pairs["two"].size());
  EXPECT_EQ("2", pairs["two"].front());

  pairs = strings::pairs("foo=1;bar=2;baz;foo=3;bam=1=2", ";&", "=");
  ASSERT_EQ(2, pairs.size());
  ASSERT_EQ(1u, pairs.count("foo"));
  ASSERT_EQ(2u, pairs["foo"].size());
  ASSERT_EQ("1", pairs["foo"].front());
  ASSERT_EQ("3", pairs["foo"].back());
  ASSERT_EQ(1u, pairs.count("bar"));
  ASSERT_EQ("2", pairs["bar"].front());
}


TEST(StringsTest, StartsWith)
{
  EXPECT_TRUE(strings::startsWith("hello world", "hello"));
  EXPECT_FALSE(strings::startsWith("hello world", "no"));
  EXPECT_FALSE(strings::startsWith("hello world", "ello"));
}


TEST(StringsTest, Contains)
{
  EXPECT_TRUE(strings::contains("hello world", "world"));
  EXPECT_FALSE(strings::contains("hello world", "no"));
}
