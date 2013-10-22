#include <gmock/gmock.h>

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

  Shared<Foo> sp(foo);

  EXPECT_EQ(10, sp->get());

  // The following won't compile.
  // sp->set(20);
}
