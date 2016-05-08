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
#ifndef __PROCESS_WINDOWS_WINSOCK_HPP__
#define __PROCESS_WINDOWS_WINSOCK_HPP__

#include <stdlib.h>

#include <glog/logging.h>

#include <stout/abort.hpp>
#include <stout/lambda.hpp>
#include <stout/windows.hpp>

#include <stout/os/socket.hpp>

#include <process/once.hpp>


namespace process {

class Winsock
{
public:
  // Initializes Winsock and calls `::exit(1)` on failure.
  Winsock() : Winsock([] { ::exit(EXIT_FAILURE); }) {}

  // Initializes Winsock and calls the specified function on failure. The intent
  // is to have the caller pass a function that will exit the process with
  // an error code (specified by the `failureCode` argument).
  Winsock(
      const lambda::function<void()>& on_failure_callback)
    : on_failure(on_failure_callback)
  {
    initialize();
  }

  ~Winsock()
  {
    net::wsa_cleanup();
  }

private:
  inline void initialize()
  {
    static Once* initialized = new Once();

    if (!initialized->once() && !net::wsa_initialize()) {
      on_failure();
    }
  }

  const lambda::function<void()> on_failure;
};

} // namespace process {

#endif // __PROCESS_WINDOWS_WINSOCK_HPP__
