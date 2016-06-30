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

#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/exit.hpp>

using process::ProcessBase;


/**
 * This process provides a target for testing remote link semantics
 * in libprocess.
 *
 * When this process starts up, it sends a message to the provided UPID.
 * This message is a notification that the test-linkee is ready to be
 * linked against and also allows the parent to discover the linkee's UPID.
 *
 * In order to test "stale" links, this process will exit upon receiving
 * a message. This gives a clear signal that a message was received.
 */
class LinkeeTestProcess : public process::Process<LinkeeTestProcess>
{
public:
  LinkeeTestProcess(const process::UPID& _parent) : parent(_parent) {}

  virtual void initialize()
  {
    send(parent, "alive", "", 0);
  }

  virtual void visit(const process::MessageEvent& event)
  {
    EXIT(EXIT_SUCCESS);
  }

private:
  const process::UPID parent;
};


// Initializes libprocess and then waits.
int main(int argc, char** argv)
{
  process::initialize();

  if (argc <= 1) {
    EXIT(EXIT_FAILURE) << "Usage: test-linkee <UPID>";
  }

  LinkeeTestProcess process(argv[1]);

  process::spawn(process);
  process::wait(process);

  return EXIT_FAILURE;
}
