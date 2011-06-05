#include <string.h>

#include <process.hpp>
#include <tuple.hpp>

#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

#include <arpa/inet.h>

#include <sys/sendfile.h>

#include "timer.hpp"

#include "call.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::runtime_error;
using std::string;
using std::map;

using process::tuple::Tuple;


enum { DO = PROCESS_MSGID, DONE };

namespace process { namespace tuple {
TUPLE(DO, ());
TUPLE(DONE, ());
}}


class Callee : public Tuple<Process>
{
protected:
  void operator () ()
  {
    do {
      switch (receive()) {
      default:
	send<DONE>(from());
      }
    } while (true);
  }
};

class Caller : public Tuple<Process>
{
private:
  int count;
  PID callee;

protected:
  void operator () ()
  {
    for (; count > 0; count--) {
      send<DO>(callee);
      receive();
    }
  }

public:
  Caller(const PID &_callee, int _count) : callee(_callee), count(_count) {}
};


int main(int argc, char **argv)
{
  if (argc < 2) {
    cerr << "usage: " << argv[0] << " <count>" << endl;
    return 1;
  }

  int count = atoi(argv[1]);

  Caller *caller = new Caller(Process::spawn(new Callee()), count);

  timer t;

  t.start("Libprocess");
  Process::wait(Process::spawn(caller));
  cout << t << endl;

  t.restart("Standard");
  for (; count > 0; count--)
    func();
  cout << t << endl;

  count = atoi(argv[1]);

  t.restart("Func VTable");
  for (; count > 0; count--)
    f->func();
  cout << t << endl;

  count = atoi(argv[1]);

  t.restart("MyFunc VTable");
  for (; count > 0; count--)
    myf->func();
  cout << t << endl;

  return 0;
}
