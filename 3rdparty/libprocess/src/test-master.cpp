/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

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
