#include "synchronized.hpp"

using std::string;


static string s1;
static synchronizable(s1);

static string s2;
static synchronizable(s2) = SYNCHRONIZED_INITIALIZER;

static string s3;
static synchronizable(s3) = SYNCHRONIZED_INITIALIZER_RECURSIVE;


void bar()
{
  synchronized(s3) {

  }
}


void foo()
{
  synchronized(s3) {
    bar();
  }
}


class Foo
{
public:
  Foo()
  {
    synchronizer(s) = SYNCHRONIZED_INITIALIZER_RECURSIVE;
  }

  void foo()
  {
    synchronized(s) {
      synchronized(s) {

      }
    }
  }
  
private:
  string s;
  synchronizable(s);
};


int main(int argc, char **argv)
{
  synchronizer(s1) = SYNCHRONIZED_INITIALIZER_RECURSIVE;
  //synchronizer(s2) = SYNCHRONIZED_INITIALIZER;

  //foo();

  Foo f;
  f.foo();

  return 0;
}
