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

#include <stout/os/pstree.hpp>

namespace os {

// Terminate the process tree rooted at the specified pid.
// Note that if the process 'pid' has exited we'll terminate the process
// tree(s) rooted at pids.
// Returns the process trees that were succesfully or unsuccessfully
// signaled. Note that the process trees can be stringified.
inline Try<std::list<ProcessTree>> killtree(
    pid_t pid,
    int signal,
    bool groups = false,
    bool sessions = false)
{
  std::list<ProcessTree> process_tree_list;
  Try<ProcessTree> process_tree = os::pstree(pid);
  if (process_tree.isError()) {
    return WindowsError();
  }

  process_tree_list.push_back(process_tree.get());

  Try<Nothing> kill_job = os::kill_job(pid);
  if (kill_job.isError())
  {
    return Error(kill_job.error());
  }
  return process_tree_list;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_KILLTREE_HPP__
