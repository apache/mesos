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

#ifndef __PROCESS_LATCH_HPP__
#define __PROCESS_LATCH_HPP__

#include <atomic>

#include <process/pid.hpp>

#include <stout/duration.hpp>

namespace process {

class Latch
{
public:
  Latch();
  virtual ~Latch();

  bool operator==(const Latch& that) const { return pid == that.pid; }
  bool operator<(const Latch& that) const { return pid < that.pid; }

  // Returns true if the latch was triggered, false if the latch had
  // already been triggered.
  bool trigger();

  // Returns true if the latch was triggered within the specified
  // duration, otherwise false.
  bool await(const Duration& duration = Seconds(-1));

private:
  // Not copyable, not assignable.
  Latch(const Latch& that);
  Latch& operator=(const Latch& that);

  std::atomic_bool triggered;
  UPID pid;
};

}  // namespace process {

#endif // __PROCESS_LATCH_HPP__
