#include <gmock/gmock.h>

#include <process/gtest.hpp>
#include <process/shared.hpp>

using namespace process;

class Foo
{
public:
  int get() const { return value; }
  void set(int _value) { value = _value; }

private:
  int value;
};


TEST(Shared, ConstAccess)
{
  Foo* foo = new Foo();
  foo->set(10);

  Shared<Foo> shared(foo);

  EXPECT_EQ(10, shared->get());

  // The following won't compile.
  // shared->set(20);
}


TEST(Shared, Null)
{
  Shared<Foo> shared(NULL);
  Shared<Foo> shared2(shared);

  EXPECT_TRUE(shared.get() == NULL);
  EXPECT_TRUE(shared2.get() == NULL);
}


TEST(Shared, Reset)
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
  EXPECT_TRUE(shared.get() == NULL);

  EXPECT_TRUE(shared2.unique());
  EXPECT_EQ(42, shared2->get());
}


TEST(Shared, Upgrade)
{
  Foo* foo = new Foo();
  foo->set(42);

  Shared<Foo> shared(foo);

  EXPECT_EQ(42, shared->get());
  EXPECT_EQ(42, (*shared).get());
  EXPECT_EQ(42, shared.get()->get());
  EXPECT_TRUE(shared.unique());

  Future<Owned<Foo> > future;

  {
    Shared<Foo> shared2(shared);

    EXPECT_EQ(42, shared2->get());
    EXPECT_EQ(42, (*shared2).get());
    EXPECT_EQ(42, shared2.get()->get());
    EXPECT_FALSE(shared2.unique());
    EXPECT_FALSE(shared.unique());

    future = shared2.upgrade();

    // A shared pointer will be reset after it called upgrade.
    EXPECT_TRUE(shared2.get() == NULL);

    // Only one upgrade is allowed.
    AWAIT_FAILED(shared.upgrade());

    // Upgrade is not done yet as 'shared' is still holding the reference.
    EXPECT_TRUE(future.isPending());
  }

  shared.reset();
  AWAIT_READY(future);

  Owned<Foo> owned = future.get();
  EXPECT_EQ(42, owned->get());
  EXPECT_EQ(42, (*owned).get());
  EXPECT_EQ(42, owned.get()->get());
}
