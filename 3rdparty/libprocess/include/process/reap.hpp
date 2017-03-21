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

#ifndef __PROCESS_REAP_HPP__
#define __PROCESS_REAP_HPP__

#include <sys/types.h>

#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/multihashmap.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>

namespace process {

// The upper bound for the poll interval in the reaper.
Duration MAX_REAP_INTERVAL();

namespace internal {

class ReaperProcess : public Process<ReaperProcess>
{
public:
  ReaperProcess();

  Future<Option<int>> reap(pid_t pid);

protected:
  virtual void initialize();

  void wait();

  void notify(pid_t pid, Result<int> status);

private:
  const Duration interval();

  multihashmap<pid_t, Owned<Promise<Option<int>>>> promises;
};


// Global reaper process. Defined in process.cpp.
extern PID<ReaperProcess> reaper;

} // namespace internal {


// Returns the exit status of the specified process if and only if
// the process is a direct child and it has not already been reaped.
// Otherwise, returns None once the process has been reaped elsewhere
// (or does not exist, which is indistinguishable from being reaped
// elsewhere). This will never fail or discard the returned future.
Future<Option<int>> reap(pid_t pid);

} // namespace process {

#endif // __PROCESS_REAP_HPP__
