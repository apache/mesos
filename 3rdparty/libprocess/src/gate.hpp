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

#ifndef __GATE_HPP__
#define __GATE_HPP__

#include <condition_variable>
#include <mutex>

#include <stout/synchronized.hpp>

namespace process {

// A Gate abstracts the concept of something that can "only happen
// once and every one else needs to queue up and wait until that thing
// happens" ... kind of like a gate that can only ever open.
//
// NOTE: historically a gate could be opened and _closed_ allowing but
// those semantics are no longer needed so they have been removed in
// order to simplify the implementation.
//
// TODO(benh): Consider removing this entirely and using `Once` for
// cleanup of a `ProcessBase` instead of a `Gate`.
class Gate
{
public:
  // Opens the gate and notifies all the waiters.
  void open()
  {
    synchronized (mutex) {
      opened = true;
      cond.notify_all();
    }
  }

  // Blocks the current thread until the gate has been opened.
  void wait()
  {
    synchronized (mutex) {
      while (!opened) {
        synchronized_wait(&cond, &mutex);
      }
    }
  }

private:
  bool opened = false;
  std::mutex mutex;
  std::condition_variable cond;
};

} // namespace process {

#endif // __GATE_HPP__
