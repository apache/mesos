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
#ifndef __STOUT_OS_WINDOWS_KILLTREE_HPP__
#define __STOUT_OS_WINDOWS_KILLTREE_HPP__

#include <stdlib.h>


namespace os {

// Sends a signal to a process tree rooted at the specified pid.
// If groups is true, this also sends the signal to all encountered
// process groups.
// If sessions is true, this also sends the signal to all encountered
// process sessions.
// Note that processes of the group and session of the parent of the
// root process is not included unless they are part of the root
// process tree.
// Note that if the process 'pid' has exited we'll signal the process
// tree(s) rooted at pids in the group or session led by the process
// if groups = true or sessions = true, respectively.
// Returns the process trees that were succesfully or unsuccessfully
// signaled. Note that the process trees can be stringified.
// TODO(benh): Allow excluding the root pid from stopping, killing,
// and continuing so as to provide a means for expressing "kill all of
// my children". This is non-trivial because of the current
// implementation.
inline Try<std::list<ProcessTree>> killtree(
    pid_t pid,
    int signal,
    bool groups = false,
    bool sessions = false)
{
  UNIMPLEMENTED;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_KILLTREE_HPP__
