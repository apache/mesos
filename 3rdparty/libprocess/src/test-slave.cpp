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

#include <test.hpp>

using namespace process::record;

class Slave : public RecordProcess
{
private:
  PID master;
  int id;

protected:
  void operator()()
  {
    send(master, pack<REGISTER>("c3po"));

    switch (receive()) {
    case OKAY: {
      std::cout << "slave registered" << std::endl;
      unpack<OKAY>(id);
      std::cout << "slave id: " << id << std::endl;
      break;
    }
    default:
      std::cout << "slave failed to register" << std::endl;
      break;
    }

    send(master, pack<UNREGISTER>(id));

    switch (receive()) {
    case OKAY:
      std::cout << "slave unregistered" << std::endl;
      break;
    default:
      std::cout << "slave failed to unregister" << std::endl;
      break;
    }

    link(master);
    switch (receive()) {
    case PROCESS_EXIT:
      std::cout << "master exited" << std::endl;
      break;
    default:
      std::cout << "unexpected message" << std::endl;
      break;
    }
  }

public:
  explicit Slave(const PID &_master) : master(_master) {}
};


int main(int argc, char **argv)
{
  PID master = make_pid(argv[1]);
  PID slave = Process::spawn(new Slave(master));
  std::cout << "slave is at " << slave << std::endl;
  Process::wait(slave);
}
