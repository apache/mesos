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

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stout/bytes.hpp>
#include <stout/gtest.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>


TEST(BytesTest, Parse)
{
  EXPECT_SOME_EQ(Terabytes(1), Bytes::parse("1TB"));
  EXPECT_SOME_EQ(Gigabytes(1), Bytes::parse("1GB"));
  EXPECT_SOME_EQ(Megabytes(1), Bytes::parse("1MB"));
  EXPECT_SOME_EQ(Kilobytes(1), Bytes::parse("1KB"));
  EXPECT_SOME_EQ(Bytes(1), Bytes::parse("1B"));

  // Cannot have fractional bytes.
  EXPECT_ERROR(Bytes::parse("1.5B"));

  // Parsing fractions is unsupported.
  EXPECT_ERROR(Bytes::parse("1.5GB"));

  // Unknown unit.
  EXPECT_ERROR(Bytes::parse("1PB"));
}


TEST(BytesTest, Arithmetic)
{
  EXPECT_EQ(Terabytes(1), Gigabytes(512) + Gigabytes(512));
  EXPECT_EQ(Terabytes(1), Terabytes(2) - Terabytes(1));

  EXPECT_EQ(Terabytes(1), Gigabytes(1) * 1024u);

  EXPECT_EQ(Gigabytes(1), Terabytes(1) / 1024u);
}


TEST(BytesTest, Comparison)
{
  EXPECT_GT(Terabytes(1), Gigabytes(1));
  EXPECT_GT(Gigabytes(1), Megabytes(1));
  EXPECT_GT(Megabytes(1), Kilobytes(1));
  EXPECT_GT(Kilobytes(1), Bytes(1));

  EXPECT_EQ(Bytes(1024), Kilobytes(1));
  EXPECT_LT(Bytes(1023), Kilobytes(1));
  EXPECT_GT(Bytes(1025), Kilobytes(1));
}


TEST(BytesTest, Stringify)
{
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
