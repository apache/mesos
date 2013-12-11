#include <gmock/gmock.h>

#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>

TEST(OptionTest, Min)
{
  Option<int> none1 = None();
  Option<int> none2 = None();
  Option<int> value1 = 10;
  Option<int> value2 = 20;

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
}


TEST(OptionTest, Max)
{
  Option<int> none1 = None();
  Option<int> none2 = None();
  Option<int> value1 = 10;
  Option<int> value2 = 20;

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
}
