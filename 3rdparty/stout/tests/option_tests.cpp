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

#include <algorithm>
#include <string>

#include <gmock/gmock.h>

#include <stout/gtest.hpp>
#include <stout/hashset.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>

using std::string;

TEST(OptionTest, Min)
{
  Option<int> none1 = None();
  Option<int> none2 = None();
  Option<int> value1 = 10;
  Option<int> value2 = 20;
  int value3 = 15;

  Option<int> result = min(none1, none2);
  ASSERT_NONE(result);

  result = min(none1, value1);
  ASSERT_SOME(result);
  EXPECT_EQ(10, result.get());

  result = min(value2, none2);
  ASSERT_SOME(result);
  EXPECT_EQ(20, result.get());

  result = min(value1, value2);
  ASSERT_SOME(result);
  EXPECT_EQ(10, result.get());

  result = min(none1, value3);
  ASSERT_SOME(result);
  EXPECT_EQ(15, result.get());

  result = min(value3, value1);
  ASSERT_SOME(result);
  EXPECT_EQ(10, result.get());
}


TEST(OptionTest, Max)
{
  Option<int> none1 = None();
  Option<int> none2 = None();
  Option<int> value1 = 10;
  Option<int> value2 = 20;
  int value3 = 15;

  Option<int> result = max(none1, none2);
  ASSERT_NONE(result);

  result = max(none1, value1);
  ASSERT_SOME(result);
  EXPECT_EQ(10, result.get());

  result = max(value2, none2);
  ASSERT_SOME(result);
  EXPECT_EQ(20, result.get());

  result = max(value1, value2);
  ASSERT_SOME(result);
  EXPECT_EQ(20, result.get());

  result = max(none1, value3);
  ASSERT_SOME(result);
  EXPECT_EQ(15, result.get());

  result = max(value3, value2);
  ASSERT_SOME(result);
  EXPECT_EQ(20, result.get());
}


TEST(OptionTest, Comparison)
{
  Option<int> none = None();
  EXPECT_NE(none, 1);
  EXPECT_FALSE(none == 1);

  Option<int> one = 1;
  EXPECT_EQ(one, 1);
  EXPECT_NE(none, one);
  EXPECT_FALSE(none == one);
  EXPECT_EQ(one, Option<int>::some(1));

  Option<int> two = 2;
  EXPECT_NE(one, two);

  Option<Option<int>> someNone = Option<Option<int>>::some(None());
  Option<Option<int>> noneNone = Option<Option<int>>::none();
  EXPECT_NE(someNone, noneNone);
  EXPECT_NE(noneNone, someNone);
}


TEST(OptionTest, NonConstReference)
{
  Option<string> s = string("hello");
  s.get() += " world";
  EXPECT_EQ("hello world", s.get());
}


TEST(OptionTest, ArrowOperator)
{
  Option<string> s = string("hello");
  EXPECT_EQ(5u, s->size());

  s->clear();
  EXPECT_TRUE(s->empty());
}


TEST(OptionTest, StarOperator)
{
  // A test class with a `moved` flag where we can verify if an object
  // has been moved.
  struct Foo
  {
    bool moved = false;
    string s;

    Foo(const string& s) { this->s = s; };

    Foo(Foo&& that)
    {
      s = std::move(that.s);
      that.moved = true;
    };

    Foo& operator=(Foo&& that)
    {
      s = std::move(that.s);
      that.moved = true;

      return *this;
    };
  };

  Option<Foo> foo("hello");
  EXPECT_EQ("hello", (*foo).s);

  Option<Foo> bar(*std::move(foo));
  EXPECT_EQ("hello", (*bar).s);
  EXPECT_TRUE(foo->moved);
}


struct NonCopyable
{
  NonCopyable() = default;
  NonCopyable(NonCopyable&&) = default;
  NonCopyable(const NonCopyable& that) = delete;
};


TEST(OptionTest, NonCopyable)
{
  Option<NonCopyable> o1(NonCopyable{});
  ASSERT_SOME(o1);

  o1 = NonCopyable();
  ASSERT_SOME(o1);

  o1 = None();
  ASSERT_NONE(o1);

  Option<NonCopyable> o2 = NonCopyable();
  ASSERT_SOME(o2);

  o2 = NonCopyable();
  ASSERT_SOME(o2);

  o2 = None();
  ASSERT_NONE(o2);

  Option<NonCopyable> o3 = None();
  ASSERT_NONE(o3);

  Option<NonCopyable> o4 = Some(NonCopyable());
  ASSERT_SOME(o4);

  o4 = Some(NonCopyable());
  ASSERT_SOME(o4);
}


TEST(OptionTest, GetOrElse)
{
  Option<string> something = string("Something");
  Option<string> none = None();
  EXPECT_EQ("Something", something.getOrElse("Else"));
  EXPECT_EQ("Else", none.getOrElse("Else"));
}


TEST(OptionTest, Hash)
{
  hashset<Option<int>> set;
  set.insert(None());

  EXPECT_EQ(1u, set.size());
  EXPECT_TRUE(set.contains(None()));

  set.insert(4);

  EXPECT_EQ(2u, set.size());
  EXPECT_TRUE(set.contains(None()));
  EXPECT_TRUE(set.contains(4));
}
