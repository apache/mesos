/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_OS_WINDOWS_PSTREE_HPP__
#define __STOUT_OS_WINDOWS_PSTREE_HPP__

#include <list>
#include <set>

#include <stout/option.hpp>
#include <stout/try.hpp>

#include <stout/os/process.hpp>


namespace os {

// Returns a process tree rooted at the specified pid using the
// specified list of processes (or an error if one occurs).
inline Try<ProcessTree> pstree(
    pid_t pid,
    const std::list<Process>& processes)
{
  UNIMPLEMENTED;
}


// Returns a process tree for the specified pid (or all processes if
// pid is none or the current process if pid is 0).
inline Try<ProcessTree> pstree(Option<pid_t> pid = None())
{
  UNIMPLEMENTED;
}


// Returns the minimum list of process trees that include all of the
// specified pids using the specified list of processes.
inline Try<std::list<ProcessTree>> pstrees(
    const std::set<pid_t>& pids,
    const std::list<Process>& processes)
{
  UNIMPLEMENTED;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_PSTREE_HPP__
