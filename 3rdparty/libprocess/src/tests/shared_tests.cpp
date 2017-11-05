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

#include <gmock/gmock.h>

#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/shared.hpp>

using process::Future;
using process::Owned;
using process::Shared;

class Foo
{
public:
  int get() const { return value; }
  void set(int _value) { value = _value; }

private:
  int value;
};


TEST(SharedTest, ConstAccess)
{
  Foo* foo = new Foo();
  foo->set(10);

  Shared<Foo> shared(foo);

  EXPECT_EQ(10, shared->get());

  // The following won't compile.
  // shared->set(20);
}


TEST(SharedTest, Null)
{
  Shared<Foo> shared(nullptr);
  Shared<Foo> shared2(shared);

  EXPECT_TRUE(shared.get() == nullptr);
  EXPECT_TRUE(shared2.get() == nullptr);
}


TEST(SharedTest, Reset)
{
  Foo* foo = new Foo();
  foo->set(42);

  Shared<Foo> shared(foo);
  Shared<Foo> shared2(shared);

  EXPECT_FALSE(shared.unique());
  EXPECT_FALSE(shared2.unique());
  EXPECT_EQ(42, shared->get());
  EXPECT_EQ(42, shared2->get());

  shared.reset();

  EXPECT_FALSE(shared.unique());
  EXPECT_TRUE(shared.get() == nullptr);

  EXPECT_TRUE(shared2.unique());
  EXPECT_EQ(42, shared2->get());
}


TEST(SharedTest, Own)
{
  Foo* foo = new Foo();
  foo->set(42);

  Shared<Foo> shared(foo);

  EXPECT_EQ(42, shared->get());
  EXPECT_EQ(42, (*shared).get());
  EXPECT_TRUE(shared.unique());

  Future<Owned<Foo>> future;

  {
    Shared<Foo> shared2(shared);

    EXPECT_EQ(42, shared2->get());
    EXPECT_EQ(42, (*shared2).get());
    EXPECT_FALSE(shared2.unique());
    EXPECT_FALSE(shared.unique());

    future = shared2.own();

    // A shared pointer will be reset after it called 'own'.
    EXPECT_TRUE(shared2.get() == nullptr);

    // Do not allow 'own' to be called twice.
    AWAIT_FAILED(shared.own());

    // Not "owned" yet as 'shared' is still holding the reference.
    EXPECT_TRUE(future.isPending());
  }

  shared.reset();
  AWAIT_READY(future);

  Owned<Foo> owned = future.get();
  EXPECT_EQ(42, owned->get());
  EXPECT_EQ(42, (*owned).get());
}
