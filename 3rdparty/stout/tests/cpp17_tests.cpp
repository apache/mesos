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

#include <stout/cpp17.hpp>

int f(int i) { return i + 101; }

TEST(Invoke, Free)
{
  // Invoke a free function.
  EXPECT_EQ(202, cpp17::invoke(f, 101));
}


TEST(Invoke, Lambda)
{
  // Invoke a lambda.
  EXPECT_EQ(42, cpp17::invoke([]() { return 42; }));
}


TEST(Invoke, Member) {
  struct S
  {
    int f(int i) const { return n + i; }
    int n;
  };

  S s{101};

  // Invoke a member function via a reference to `S`.
  EXPECT_EQ(303, cpp17::invoke(&S::f, s, 202));

  // Invoke a member function via a pointer to `S`.
  EXPECT_EQ(303, cpp17::invoke(&S::f, &s, 202));

  // Invoke (access) a data member via a reference to `S`.
  EXPECT_EQ(101, cpp17::invoke(&S::n, s));

  // Invoke (access) a data member via a pointer to `S`.
  EXPECT_EQ(101, cpp17::invoke(&S::n, &s));
}


TEST(Invoke, FunctionObject)
{
  // Invoke a function object.
  struct F
  {
    int operator()(int i) const { return i + 202; }
  };

  EXPECT_EQ(404, cpp17::invoke(F{}, 202));
}
