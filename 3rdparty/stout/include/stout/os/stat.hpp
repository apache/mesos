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

#ifndef __STOUT_OS_STAT_HPP__
#define __STOUT_OS_STAT_HPP__

namespace os {

namespace stat {

// Specify whether symlink path arguments should be followed or
// not. APIs in the os::stat family that take a FollowSymlink
// argument all provide FOLLOW_SYMLINK as the default value,
// so they will follow symlinks unless otherwise specified.
enum FollowSymlink
{
  DO_NOT_FOLLOW_SYMLINK,
  FOLLOW_SYMLINK
};

} // namespace stat {

} // namespace os {

// For readability, we minimize the number of #ifdef blocks in the code by
// splitting platform specific system calls into separate directories.
#ifdef __WINDOWS__
#include <stout/os/windows/stat.hpp>
#else
#include <stout/os/posix/stat.hpp>
#endif // __WINDOWS__


#endif // __STOUT_OS_STAT_HPP__
