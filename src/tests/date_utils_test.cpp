#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <common/date_utils.hpp>

using namespace mesos::internal;


TEST(DateUtilsTest, humanReadable)
{
  DateUtils::setMockDate("200102030405");
  ASSERT_EQ("200102030405", DateUtils::humanReadableDate());
}

TEST(DateUtilsTest, currentDateInMicro)
{
  DateUtils::setMockDate("200102030405");
  ASSERT_EQ(981173100000000LL, DateUtils::currentDateInMicro());
}
