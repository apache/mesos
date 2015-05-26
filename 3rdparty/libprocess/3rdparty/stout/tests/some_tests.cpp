#include <gtest/gtest.h>

#include <map>
#include <string>

#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/some.hpp>
#include <stout/try.hpp>

TEST(Stout, Some)
{
  Option<int> o1 = Some(42);
  EXPECT_SOME(o1);
  EXPECT_EQ(42, o1.get());

  Result<int> r1 = Some(42);
  EXPECT_SOME(r1);
  EXPECT_EQ(42, r1.get());

  Try<Option<int> > t1 = Some(42);
  ASSERT_SOME(t1);
  EXPECT_SOME(t1.get());
  EXPECT_EQ(42, t1.get().get());
  t1 = None();
  ASSERT_SOME(t1);
  EXPECT_NONE(t1.get());

  Try<Result<int> > t2 = Some(42);
  ASSERT_SOME(t2);
  EXPECT_SOME(t2.get());
  EXPECT_EQ(42, t2.get().get());

  Option<Result<int> > o2 = Some(42);
  ASSERT_SOME(o2);
  EXPECT_SOME(o2.get());
  EXPECT_EQ(42, o2.get().get());

  Option<Result<int> > o3 = Some(Some(42));
  ASSERT_SOME(o3);
  EXPECT_SOME(o3.get());
  EXPECT_EQ(42, o3.get().get());

  Result<Option<int> > r2 = Some(42);
  ASSERT_SOME(r2);
  EXPECT_SOME(r2.get());
  EXPECT_EQ(42, r2.get().get());

  Result<Option<int> > r3 = Some(Some(42));
  ASSERT_SOME(r3);
  EXPECT_SOME(r3.get());
  EXPECT_EQ(42, r3.get().get());

  Option<std::string> o4 = Some("hello");
  EXPECT_SOME(o4);
  EXPECT_EQ("hello", o4.get());

  Result<std::string> r4 = Some("world");
  EXPECT_SOME(r4);
  EXPECT_EQ("world", r4.get());

  std::map<std::string, Option<std::string> > values;
  values["no-debug"] = None();
  values["debug"] = None();
  values["debug"] = Some("true");
  values["debug"] = Some("false");
  values["name"] = Some("frank");
}
