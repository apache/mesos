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

#ifndef __PROCESS_ONCE_HPP__
#define __PROCESS_ONCE_HPP__

#include <condition_variable>
#include <mutex>

#include <stout/synchronized.hpp>

namespace process {

// Provides a _blocking_ abstraction that's useful for performing a
// task exactly once.
class Once
{
public:
  Once() : started(false), finished(false) {}

  ~Once() = default;

  // Returns true if this Once instance has already transitioned to a
  // 'done' state (i.e., the action you wanted to perform "once" has
  // been completed). Note that this BLOCKS until Once::done has been
  // called.
  bool once()
  {
    bool result = false;

    synchronized (mutex) {
      if (started) {
        while (!finished) {
          synchronized_wait(&cond, &mutex);
        }
        result = true;
      } else {
        started = true;
      }
    }

    return result;
  }

  // Transitions this Once instance to a 'done' state.
  void done()
  {
    synchronized (mutex) {
      if (started && !finished) {
        finished = true;
        cond.notify_all();
      }
    }
  }

private:
  // Not copyable, not assignable.
  Once(const Once& that);
  Once& operator=(const Once& that);

  std::mutex mutex;
  std::condition_variable cond;
  bool started;
  bool finished;
};

}  // namespace process {

#endif // __PROCESS_ONCE_HPP__
