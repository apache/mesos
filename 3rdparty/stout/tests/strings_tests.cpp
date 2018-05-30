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

#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stout/format.hpp>
#include <stout/gtest.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

using std::map;
using std::string;
using std::vector;


TEST(StringsTest, Format)
{
  Try<string> result = strings::format("%s %s", "hello", "world");
  EXPECT_SOME_EQ("hello world", result);

  result = strings::format("hello %d", 42);
  EXPECT_SOME_EQ("hello 42", result);

  result = strings::format("hello %s", "fourty-two");
  EXPECT_SOME_EQ("hello fourty-two", result);

  string hello = "hello";
  result = strings::format("%s %s", hello, "fourty-two");
  EXPECT_SOME_EQ("hello fourty-two", result);
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

  // Also test trimming from just the prefix or suffix.
  EXPECT_EQ("hello world\t \n\r ",
            strings::trim(" \t hello world\t \n\r ", strings::PREFIX));
  EXPECT_EQ(" \t hello world",
            strings::trim(" \t hello world\t \n\r ", strings::SUFFIX));
  EXPECT_EQ("\t hello world\t \n\r ",
            strings::trim(" \t hello world\t \n\r ", strings::PREFIX, " "));
  EXPECT_EQ(" \t hello world\t \n",
            strings::trim(" \t hello world\t \n\r ", strings::SUFFIX, " \r"));
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
  ASSERT_TRUE(tokens.empty());
}


TEST(StringsTest, TokenizeDelimOnlyString)
{
  vector<string> tokens = strings::tokenize("   ", " ");
  ASSERT_TRUE(tokens.empty());
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


TEST(StringsTest, TokenizeNZero)
{
  vector<string> tokens = strings::tokenize("foo,bar,,,", ",", 0);
  ASSERT_TRUE(tokens.empty());
}


TEST(StringsTest, TokenizeNOne)
{
  vector<string> tokens = strings::tokenize("foo,bar,,,", ",", 1);
  ASSERT_EQ(1u, tokens.size());
  EXPECT_EQ("foo,bar,,,", tokens[0]);
}


TEST(StringsTest, TokenizeNDelimOnlyString)
{
  vector<string> tokens = strings::tokenize(",,,", ",", 2);
  ASSERT_TRUE(tokens.empty());
}


TEST(StringsTest, TokenizeN)
{
  vector<string> tokens = strings::tokenize("foo,bar,,baz", ",", 2);
  ASSERT_EQ(2u, tokens.size());
  EXPECT_EQ("foo",      tokens[0]);
  EXPECT_EQ("bar,,baz", tokens[1]);
}


TEST(StringsTest, TokenizeNStringWithDelimsAtStart)
{
  vector<string> tokens = strings::tokenize(",,foo,bar,,baz", ",", 5);
  ASSERT_EQ(3u, tokens.size());
  EXPECT_EQ("foo", tokens[0]);
  EXPECT_EQ("bar", tokens[1]);
  EXPECT_EQ("baz", tokens[2]);
}


TEST(StringsTest, TokenizeNStringWithDelimsAtEnd)
{
  vector<string> tokens = strings::tokenize("foo,bar,,baz,,", ",", 4);
  ASSERT_EQ(3u, tokens.size());
  EXPECT_EQ("foo", tokens[0]);
  EXPECT_EQ("bar", tokens[1]);
  EXPECT_EQ("baz", tokens[2]);
}


TEST(StringsTest, TokenizeNStringWithDelimsAtStartAndEnd)
{
  vector<string> tokens = strings::tokenize(",,foo,bar,,", ",", 6);
  ASSERT_EQ(2u, tokens.size());
  EXPECT_EQ("foo", tokens[0]);
  EXPECT_EQ("bar", tokens[1]);
}


TEST(StringsTest, TokenizeNWithMultipleDelims)
{
  vector<string> tokens = strings::tokenize("foo.bar,.,.baz.", ",.", 6);
  ASSERT_EQ(3u, tokens.size());
  EXPECT_EQ("foo", tokens[0]);
  EXPECT_EQ("bar", tokens[1]);
  EXPECT_EQ("baz", tokens[2]);
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


TEST(StringsTest, SplitNZero)
{
  vector<string> tokens = strings::split("foo,bar,,,", ",", 0);
  ASSERT_TRUE(tokens.empty());
}


TEST(StringsTest, SplitNOne)
{
  vector<string> tokens = strings::split("foo,bar,,,", ",", 1);
  ASSERT_EQ(1u, tokens.size());
  EXPECT_EQ("foo,bar,,,", tokens[0]);
}


TEST(StringsTest, SplitNDelimOnlyString)
{
  vector<string> tokens = strings::split(",,,", ",", 2);
  ASSERT_EQ(2u, tokens.size());
  EXPECT_EQ("",   tokens[0]);
  EXPECT_EQ(",,", tokens[1]);
}


TEST(StringsTest, SplitN)
{
  vector<string> tokens = strings::split("foo,bar,,baz", ",", 2);
  ASSERT_EQ(2u, tokens.size());
  EXPECT_EQ("foo",      tokens[0]);
  EXPECT_EQ("bar,,baz", tokens[1]);
}


TEST(StringsTest, SplitNStringWithDelimsAtStart)
{
  vector<string> tokens = strings::split(",,foo,bar,,baz", ",", 5);
  ASSERT_EQ(5u, tokens.size());
  EXPECT_EQ("",     tokens[0]);
  EXPECT_EQ("",     tokens[1]);
  EXPECT_EQ("foo",  tokens[2]);
  EXPECT_EQ("bar",  tokens[3]);
  EXPECT_EQ(",baz", tokens[4]);
}


TEST(StringsTest, SplitNStringWithDelimsAtEnd)
{
  vector<string> tokens = strings::split("foo,bar,,baz,,", ",", 4);
  ASSERT_EQ(4u, tokens.size());
  EXPECT_EQ("foo",   tokens[0]);
  EXPECT_EQ("bar",   tokens[1]);
  EXPECT_EQ("",      tokens[2]);
  EXPECT_EQ("baz,,", tokens[3]);
}


TEST(StringsTest, SplitNStringWithDelimsAtStartAndEnd)
{
  vector<string> tokens = strings::split(",,foo,bar,,", ",", 6);
  ASSERT_EQ(6u, tokens.size());
  EXPECT_EQ("",    tokens[0]);
  EXPECT_EQ("",    tokens[1]);
  EXPECT_EQ("foo", tokens[2]);
  EXPECT_EQ("bar", tokens[3]);
  EXPECT_EQ("",    tokens[4]);
  EXPECT_EQ("",    tokens[5]);
}


TEST(StringsTest, SplitNWithMultipleDelims)
{
  vector<string> tokens = strings::split("foo.bar,.,.baz.", ",.", 6);
  ASSERT_EQ(6u, tokens.size());
  EXPECT_EQ("foo",  tokens[0]);
  EXPECT_EQ("bar",  tokens[1]);
  EXPECT_EQ("",     tokens[2]);
  EXPECT_EQ("",     tokens[3]);
  EXPECT_EQ("",     tokens[4]);
  EXPECT_EQ("baz.", tokens[5]);
}


TEST(StringsTest, Pairs)
{
  {
    map<string, vector<string>> pairs = strings::pairs("one=1,two=2", ",", "=");
    map<string, vector<string>> expected = {
      {"one", {"1"}},
      {"two", {"2"}}
    };

    EXPECT_EQ(expected, pairs);
  }

  {
    map<string, vector<string>> pairs = strings::pairs(
        "foo=1;bar=2;baz;foo=3;bam=1=2", ";&", "=");

    map<string, vector<string>> expected = {
      {"foo", {"1", "3"}},
      {"bar", {"2"}}
    };

    EXPECT_EQ(expected, pairs);
  }
}


TEST(StringsTest, Join)
{
  EXPECT_EQ("a/b", strings::join("/", "a", "b"));
  EXPECT_EQ("a/b/c", strings::join("/", "a", "b", "c"));
  EXPECT_EQ("a\nb\nc\nd", strings::join("\n", "a", "b", "c", "d"));
  EXPECT_EQ("a//b///d", strings::join("/", "a", "", "b", "", "", "d"));

  std::stringstream ss;
  EXPECT_EQ("a, b, c", strings::join(ss, ", ", "a", "b", "c").str());

  const string gnarly("gnarly");
  EXPECT_EQ("a/gnarly/c", strings::join("/", "a", gnarly, "c"));

  const bool is_true = true;
  const std::set<int32_t> my_set {1, 2, 3};
  EXPECT_EQ(
      "a/gnarly/true/{ 1, 2, 3 }/c",
      strings::join("/", "a", gnarly, is_true, my_set, "c"));
}


TEST(StringsTest, StartsWith)
{
  EXPECT_TRUE(strings::startsWith("hello world", "hello"));
  EXPECT_FALSE(strings::startsWith("hello world", "no"));
  EXPECT_FALSE(strings::startsWith("hello world", "ello"));

  EXPECT_TRUE(strings::startsWith("hello world", std::string("hello")));
  EXPECT_FALSE(strings::startsWith("hello world", std::string("ello")));
}


TEST(StringsTest, EndsWith)
{
  EXPECT_TRUE(strings::endsWith("hello world", "world"));
  EXPECT_FALSE(strings::endsWith("hello world", "no"));
  EXPECT_FALSE(strings::endsWith("hello world", "ello"));

  EXPECT_TRUE(strings::endsWith("hello world", std::string("world")));
  EXPECT_FALSE(strings::endsWith("hello world", std::string("worl")));
}


TEST(StringsTest, Contains)
{
  EXPECT_TRUE(strings::contains("hello world", "world"));
  EXPECT_FALSE(strings::contains("hello world", "no"));
}
