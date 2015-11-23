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

#include <process/id.hpp>
#include <process/latch.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>

namespace process {

// TODO(benh): Provide an "optimized" implementation of a latch that
// is libprocess aware. That is, allow integrate "waiting" on a latch
// within libprocess such that it doesn't cost a memory allocation, a
// spawn, a message send, a wait, and two user-space context-switchs.

Latch::Latch() : triggered(false)
{
  // Deadlock is possible if one thread is trying to delete a latch
  // but the libprocess thread(s) is trying to acquire a resource the
  // deleting thread is holding. Hence, we only save the PID for
  // triggering the latch and let the GC actually do the deleting
  // (thus no waiting is necessary, and deadlocks are avoided).
  pid = spawn(new ProcessBase(ID::generate("__latch__")), true);
}


Latch::~Latch()
{
  bool expected = false;
  if (triggered.compare_exchange_strong(expected, true)) {
    terminate(pid);
  }
}


bool Latch::trigger()
{
  bool expected = false;
  if (triggered.compare_exchange_strong(expected, true)) {
    terminate(pid);
    return true;
  }
  return false;
}


bool Latch::await(const Duration& duration)
{
  if (!triggered.load()) {
    process::wait(pid, duration); // Explict to disambiguate.
    // It's possible that we failed to wait because:
    //   (1) Our process has already terminated.
    //   (2) We timed out (i.e., duration was not "infinite").

    // In the event of (1) we might need to return 'true' since a
    // terminated process might imply that the latch has been
    // triggered. To capture this we simply return the value of
    // 'triggered' (which will also capture cases where we actually
    // timed out but have since triggered, which seems like an
    // acceptable semantics given such a "tie").
    return triggered.load();
  }

  return true;
}

} // namespace process {
