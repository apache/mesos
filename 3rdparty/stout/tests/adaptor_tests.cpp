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

#include <vector>

#include <gtest/gtest.h>

#include <stout/adaptor.hpp>
#include <stout/foreach.hpp>

using std::vector;


TEST(AdaptorTest, Reversed)
{
  vector<int> input = {1, 2, 3};
  vector<int> output;

  foreach (int i, adaptor::reverse(input)) {
    output.push_back(i);
  }

  ASSERT_EQ(3u, output.size());
  EXPECT_EQ(3, output[0]);
  EXPECT_EQ(2, output[1]);
  EXPECT_EQ(1, output[2]);
}
