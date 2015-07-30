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
#ifndef __STOUT_OS_WINDOWS_EXISTS_HPP__
#define __STOUT_OS_WINDOWS_EXISTS_HPP__

#include <string>


namespace os {

inline bool exists(const std::string& path)
{
  UNIMPLEMENTED;
}


// Determine if the process identified by pid exists.
// NOTE: Zombie processes have a pid and therefore exist. See os::process(pid)
// to get details of a process.
inline bool exists(pid_t pid)
{
  UNIMPLEMENTED;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_EXISTS_HPP__
