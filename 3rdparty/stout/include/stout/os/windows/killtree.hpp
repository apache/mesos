// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_OS_WINDOWS_KILLTREE_HPP__
#define __STOUT_OS_WINDOWS_KILLTREE_HPP__

#include <stdlib.h>

#include <stout/os.hpp>

#include <stout/os.hpp>       // For `kill_job`.
#include <stout/try.hpp>      // For `Try<>`.
#include <stout/windows.hpp>  // For `SharedHandle`.

namespace os {

// Terminate the "process tree" rooted at the specified pid.
// Since there is no process tree concept on Windows,
// internally this function looks up the job object for the given pid
// and terminates the job. This is possible because `name_job`
// provides an idempotent one-to-one mapping from pid to name.
inline Try<std::list<ProcessTree>> killtree(
    pid_t pid,
    int signal,
    bool groups = false,
    bool sessions = false)
{
  Try<std::wstring> name = os::name_job(pid);
  if (name.isError()) {
    return Error("Failed to determine job object name: " + name.error());
  }

  Try<SharedHandle> handle =
    os::open_job(JOB_OBJECT_TERMINATE, false, name.get());
  if (handle.isError()) {
    return Error("Failed to open job object: " + handle.error());
  }

  Try<Nothing> killJobResult = os::kill_job(handle.get());
  if (killJobResult.isError()) {
    return Error("Failed to delete job object: " + killJobResult.error());
  }

  // NOTE: This return value is unused. A future refactor
  // may change the return type to `Try<None>`.
  std::list<ProcessTree> process_tree_list;
  return process_tree_list;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_KILLTREE_HPP__
