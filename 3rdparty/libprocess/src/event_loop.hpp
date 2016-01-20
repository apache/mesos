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

#ifndef __EVENT_LOOP_HPP__
#define __EVENT_LOOP_HPP__

#include <stout/duration.hpp>
#include <stout/lambda.hpp>

namespace process {

// The interface that must be implemented by an event management
// system. This is a class to cleanly isolate the interface and so
// that in the future we can support multiple implementations.
class EventLoop
{
public:
  // Initializes the event loop.
  static void initialize();

  // Invoke the specified function in the event loop after the
  // specified duration.
  // TODO(bmahler): Update this to use rvalue references.
  static void delay(
      const Duration& duration,
      const lambda::function<void()>& function);

  // Returns the current time w.r.t. the event loop.
  static double time();

  // Runs the event loop.
  static void run();

  // Asynchronously tells the event loop to stop and then returns.
  static void stop();
};

} // namespace process {

#endif // __EVENT_LOOP_HPP__
