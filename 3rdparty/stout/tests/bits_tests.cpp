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

#include <stout/bits.hpp>

TEST(BitsTest, CountSetBits)
{
  EXPECT_EQ(0, bits::countSetBits(0));
  EXPECT_EQ(6, bits::countSetBits(0xf3));
  EXPECT_EQ(15, bits::countSetBits(0xffbf));
  EXPECT_EQ(22, bits::countSetBits(0xfffffc));
  EXPECT_EQ(26, bits::countSetBits(0xfffffcf));
  EXPECT_EQ(32, bits::countSetBits(0xffffffff));
}
