#include <string>

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>

using std::string;


None none1()
{
  return None();
}


Option<string> none2()
{
  return None();
}


Option<string> none3(const Option<string>& o)
{
  return o;
}


Result<string> none4()
{
  return None();
}


Result<string> none5(const Result<string>& r)
{
  return r;
}


TEST(NoneTest, Test)
{
  Option<string> o = none1();
  EXPECT_TRUE(o.isNone());
  o = none2();
  EXPECT_TRUE(o.isNone());
  o = none3(none1());
  EXPECT_TRUE(o.isNone());

  Result<string> r = none1();
  EXPECT_TRUE(r.isNone());
  r = none4();
  EXPECT_TRUE(r.isNone());
  r = none5(none1());
  EXPECT_TRUE(r.isNone());
}
