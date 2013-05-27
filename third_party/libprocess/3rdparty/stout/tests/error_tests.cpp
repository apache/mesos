#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <string>

#include <stout/error.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

using std::string;


Error error1()
{
  return Error("Failed to ...");
}


Try<string> error2()
{
  return Error("Failed to ...");
}


Try<string> error3(const Try<string>& t)
{
  return t;
}


Result<string> error4()
{
  return Error("Failed to ...");
}


Result<string> error5(const Result<string>& r)
{
  return r;
}


TEST(ErrorTest, Test)
{
  Try<string> t = error1();
  EXPECT_TRUE(t.isError());
  t = error2();
  EXPECT_TRUE(t.isError());
  t = error3(error1());
  EXPECT_TRUE(t.isError());

  Result<string> r = error1();
  EXPECT_TRUE(r.isError());
  r = error4();
  EXPECT_TRUE(r.isError());
  r = error5(error1());
  EXPECT_TRUE(r.isError());
}
