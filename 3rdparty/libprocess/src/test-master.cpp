#include <io.hpp>
#include <tuple.hpp>

#include <string>

#include "test.hpp"

using std::string;


using namespace process::tuple;


class Master : public Tuple<Process>
{
private:
  int id;

protected:
  void operator () ()
  {
    do {
      switch (receive()) {
      case REGISTER: {
        Out::println("Master received REGISTER");

        string name;
        unpack<REGISTER>(name);

        Out::println("Registered slave: %s", name.c_str());

        send(from(), pack<OKAY>(id++));
        break;
      }
      case UNREGISTER: {
        Out::println("Master received UNREGISTER");

        int slave_id;
        unpack<UNREGISTER>(slave_id);

        Out::println("Unregistered slave id: %d", slave_id);

        send(from(), pack<OKAY>(0));
        break;
      }
      default:
        Out::println("UNKNOWN MESSAGE RECEIVED");
      }
    } while (true);
  }

public:
  Master() : id(0) {}
};


int main(int argc, char **argv)
{
  PID master = Process::spawn(new Master());
  Out::println("master: %s", string(master).c_str());
  Process::wait(master);
}
