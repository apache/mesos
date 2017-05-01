// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <limits>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/numify.hpp>


TEST(NumifyTest, DecNumberTest)
{
  Try<unsigned int> num1 = numify<unsigned int>("10");
  EXPECT_SOME_EQ(10u, num1);

  Try<int> num2 = numify<int>("-10");
  EXPECT_SOME_EQ(-10, num2);

  // `numify<T>` does not return an error if T is an unsigned integral
  // type and the input is a negative number; rather, it returns the
  // result of casting the numeric input value to T.
  Try<unsigned int> num3 = numify<unsigned int>("-1");
  EXPECT_SOME_EQ(std::numeric_limits<unsigned int>::max(), num3);

  EXPECT_ERROR(numify<unsigned int>(""));
  EXPECT_ERROR(numify<int>("-10."));
  EXPECT_ERROR(numify<unsigned int>("123xyz"));
}


TEST(NumifyTest, HexNumberTest)
{
  Try<unsigned int> num1 = numify<unsigned int>("0xdeadbeef");
  EXPECT_SOME_EQ(0xdeadbeefu, num1);

  Try<unsigned int> num2 = numify<unsigned int>("0x10");
  EXPECT_SOME_EQ(16u, num2);

  // Keep it consistent with the handling of non-hex numbers.
  Try<int> num3 = numify<int>("-0x10");
  EXPECT_SOME_EQ(-0x10, num3);

  EXPECT_ERROR(numify<unsigned int>(""));
  EXPECT_ERROR(numify<unsigned int>("0xxyz"));
  EXPECT_ERROR(numify<unsigned int>("abc"));
  EXPECT_ERROR(numify<unsigned int>("0x0x1"));
  EXPECT_ERROR(numify<double>("0x10.9"));
  EXPECT_ERROR(numify<double>("0x1p-5"));
}
