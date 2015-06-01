#include <gmock/gmock.h>

#include <string>
#include <stout/gtest.hpp>
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

  Option<Option<int> > someNone = Option<Option<int> >::some(None());
  Option<Option<int> > noneNone = Option<Option<int> >::none();
  EXPECT_NE(someNone, noneNone);
  EXPECT_NE(noneNone, someNone);
}


TEST(OptionTest, NonConstReference)
{
  Option<string> s = string("hello");
  s.get() += " world";
  EXPECT_EQ("hello world", s.get());
}
