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

#ifndef __STOUT_OS_WINDOWS_DIRENTSIZE_HPP__
#define __STOUT_OS_WINDOWS_DIRENTSIZE_HPP__

#include <stout/internal/windows/dirent.hpp>

#include <stout/windows.hpp>


namespace os {

inline size_t dirent_size(DIR* dir)
{
  // NOTE: Size calculation logic here is much simpler than on POSIX because
  // our implementation of `dirent` is constant-sized. In particular, on POSIX,
  // we usually have to calculate the maximum name size for a path before we
  // can alloc a correctly-size `dirent`, but on Windows, `dirent.d_name` is
  // always `MAX_PATH` bytes in size.
  //
  // This follows closely from the Windows standard API data structures for
  // manipulating and querying directories. For example, the structures
  // `WIN32_FIND_DATA`[1] (which in many ways is the Windows equivalent of
  // `dirent`) has a field `cFileName` (which is much like `d_name`) that is
  // also `MAX_PATH` in size.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa365740(v=vs.85).aspx
  return sizeof(dirent);
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_DIRENTSIZE_HPP__
