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

#ifndef __PROCESS_COUNT_DOWN_LATCH_HPP__
#define __PROCESS_COUNT_DOWN_LATCH_HPP__

#include <atomic>

#include <process/future.hpp>

namespace process {

// An implementation of a count down latch that returns a Future to
// signify when it gets triggered.
class CountDownLatch
{
public:
  CountDownLatch(size_t count = 1) : count(count) {}

  void decrement()
  {
    size_t expected = count.load();
    while (expected > 0) {
      if (count.compare_exchange_strong(expected, expected - 1)) {
        if (expected == 1) {
          promise.set(Nothing());
        }
        break;
      }
    }
  }

  Future<Nothing> triggered()
  {
    return promise.future();
  }

private:
  std::atomic<size_t> count;
  Promise<Nothing> promise;
};

} // namespace process {

#endif // __PROCESS_COUNT_DOWN_LATCH_HPP__
