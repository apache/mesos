#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stout/bytes.hpp>
#include <stout/gtest.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>


TEST(Stout, Bytes)
{
  Try<Bytes> _1terabyte = Bytes::parse("1TB");

  EXPECT_SOME_EQ(Terabytes(1), _1terabyte);
  EXPECT_SOME_EQ(Gigabytes(1024), _1terabyte);
  EXPECT_SOME_EQ(Megabytes(1024 * 1024), _1terabyte);
  EXPECT_SOME_EQ(Kilobytes(1024 * 1024 * 1024), _1terabyte);
  EXPECT_SOME_EQ(Bytes(1024LLU * 1024 * 1024 * 1024), _1terabyte);

  EXPECT_EQ(Bytes(1024), Kilobytes(1));
  EXPECT_LT(Bytes(1023), Kilobytes(1));
  EXPECT_GT(Bytes(1025), Kilobytes(1));

  EXPECT_NE(Megabytes(1023), Gigabytes(1));

  EXPECT_EQ("0B", stringify(Bytes()));

  EXPECT_EQ("1KB", stringify(Kilobytes(1)));
  EXPECT_EQ("1MB", stringify(Megabytes(1)));
  EXPECT_EQ("1GB", stringify(Gigabytes(1)));
  EXPECT_EQ("1TB", stringify(Terabytes(1)));

  EXPECT_EQ("1023B", stringify(Bytes(1023)));
  EXPECT_EQ("1023KB", stringify(Kilobytes(1023)));
  EXPECT_EQ("1023MB", stringify(Megabytes(1023)));
  EXPECT_EQ("1023GB", stringify(Gigabytes(1023)));
}
