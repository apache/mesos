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

#include <gmock/gmock.h>

#include <stout/error.hpp>
#include <stout/gtest.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

using std::string;

// Verify Try to Result conversion.
TEST(ResultTest, TryToResultConversion)
{
  // Test with implicit Result(Try&) contructor.
  Try<int> foo = 5;
  Result<int> bar = foo;
  EXPECT_SOME(bar);
  EXPECT_EQ(bar.get(), foo.get());

  // Test with explicit constructor.
  Result<int> result = None();
  result = Result<int>(foo);
  EXPECT_SOME(result);
  EXPECT_EQ(result.get(), foo.get());

  // Test Try with error state.
  Try<int> tryError(Error("Invalid Try"));
  Result<int> resultError = tryError;
  EXPECT_ERROR(resultError);
  EXPECT_EQ(tryError.error(), resultError.error());

  // Test Try with error state with explicit constructor.
  result = Result<int>(tryError);
  EXPECT_ERROR(result);
  EXPECT_EQ(result.error(), tryError.error());
}


TEST(ResultTest, ArrowOperator)
{
  Result<string> s = string("hello");
  EXPECT_EQ(5u, s->size());

  s->clear();
  EXPECT_TRUE(s->empty());
}


TEST(ResultTest, StarOperator)
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

  Result<Foo> foo("hello");
  EXPECT_EQ("hello", (*foo).s);

  Result<Foo> bar(*std::move(foo));
  EXPECT_EQ("hello", (*bar).s);
  EXPECT_TRUE(foo->moved);
}
