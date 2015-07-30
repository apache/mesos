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

#ifndef __STOUT_OS_WINDOWS_FCNTL_HPP__
#define __STOUT_OS_WINDOWS_FCNTL_HPP__

#include <stout/nothing.hpp>
#include <stout/try.hpp>


namespace os {

inline Try<Nothing> cloexec(int fd)
{
  UNIMPLEMENTED;
}


inline Try<bool> isCloexec(int fd)
{
  UNIMPLEMENTED;
}


inline Try<Nothing> nonblock(int fd)
{
  UNIMPLEMENTED;
}


inline Try<bool> isNonblock(int fd)
{
  UNIMPLEMENTED;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_FCNTL_HPP__
