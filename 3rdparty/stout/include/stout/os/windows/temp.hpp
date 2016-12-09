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

#ifndef __STOUT_OS_WINDOWS_TEMP_HPP__
#define __STOUT_OS_WINDOWS_TEMP_HPP__

#include <string>

#include <stout/windows.hpp>


namespace os {

// Attempts to resolve the system-designated temporary directory before
// falling back to a sensible default. On Windows, this involves checking
// (in this order) environment variables for `TMP`, `TEMP`, and `USERPROFILE`
// followed by the Windows directory (`::GetTimePath`).  In the unlikely event
// where none of these are found, this function returns the current directory.
inline std::string temp()
{
  char temp_folder[MAX_PATH + 2];
  if (::GetTempPath(MAX_PATH + 2, temp_folder) == 0) {
    if (::GetCurrentDirectory(MAX_PATH + 2, temp_folder) == 0) {
      // Failed, use relative path.
      return ".";
    }
  }

  return std::string(temp_folder);
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_TEMP_HPP__
