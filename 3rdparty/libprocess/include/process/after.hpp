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

#ifndef __PROCESS_AFTER_HPP__
#define __PROCESS_AFTER_HPP__

#include <memory>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/timer.hpp>

#include <stout/duration.hpp>
#include <stout/nothing.hpp>

namespace process {

// Provides an abstraction over `Timer` and `Clock::timer` that
// completes a future after some duration and lets you attempt to
// discard that future.
//
// This can be used along with `loop` to create "waiting loops", for
// example:
//
//   // Wait until a file exists, check for it ever every second.
//   loop(None(),
//        []() {
//          return after(Seconds(1));
//        },
//        [=]() {
//          if (os::exists(file)) -> ControlFlow<Nothing> {
//            return Break();
//          }
//          return Continue();
//        });
inline Future<Nothing> after(const Duration& duration)
{
  std::shared_ptr<Promise<Nothing>> promise(new Promise<Nothing>());

  Timer timer = Clock::timer(duration, [=]() {
    promise->set(Nothing());
  });

  // Attempt to discard the promise if the future is discarded.
  //
  // NOTE: while the future holds a reference to the promise there is
  // no cicular reference here because even if there are no references
  // to the Future the timer will eventually fire and we'll set the
  // promise which will clear the `onDiscard` callback and delete the
  // reference to Promise.
  promise->future()
    .onDiscard([=]() {
      if (Clock::cancel(timer)) {
        promise->discard();
      }
    });

  return promise->future();
}

} // namespace process {

#endif // __PROCESS_AFTER_HPP__
