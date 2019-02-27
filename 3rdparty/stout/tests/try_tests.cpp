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

#include <string>

#include <gtest/gtest.h>

#include <stout/try.hpp>

using std::string;

TEST(TryTest, ArrowOperator)
{
  Try<string> s = string("hello");
  EXPECT_EQ(5u, s->size());

  s->clear();
  EXPECT_TRUE(s->empty());
}


TEST(TryTest, StarOperator)
{
  // A test class with a `moved` flag where we can verify if an object
  // has been moved.
  struct Foo
  {
    bool moved = false;
    string s;

    Foo(const string& s) { this->s = s; };

    Foo(Foo&& that)
    {
      s = std::move(that.s);
      that.moved = true;
    };

    Foo& operator=(Foo&& that)
    {
      s = std::move(that.s);
      that.moved = true;

      return *this;
    };
  };

  Try<Foo> foo("hello");
  EXPECT_EQ("hello", (*foo).s);

  Try<Foo> bar(*std::move(foo));
  EXPECT_EQ("hello", (*bar).s);
  EXPECT_TRUE(foo->moved);
}
