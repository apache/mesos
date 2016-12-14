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

#ifndef __STOUT_OS_WINDOWS_PIPE_HPP__
#define __STOUT_OS_WINDOWS_PIPE_HPP__

#include <array>

#include <stout/error.hpp>
#include <stout/try.hpp>

namespace os {

// Create pipes for interprocess communication. Since the pipes cannot
// be used directly by Posix `read/write' functions they are wrapped
// in file descriptors, a process-local concept.
inline Try<std::array<WindowsFD, 2>> pipe()
{
  // Create inheritable pipe, as described in MSDN[1].
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/aa365782(v=vs.85).aspx
  SECURITY_ATTRIBUTES securityAttr;
  securityAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  securityAttr.bInheritHandle = TRUE;
  securityAttr.lpSecurityDescriptor = nullptr;

  HANDLE read_handle;
  HANDLE write_handle;

  const BOOL result =
    ::CreatePipe(&read_handle, &write_handle, &securityAttr, 0);

  if (!result) {
    return WindowsError();
  }

  return std::array<WindowsFD, 2>{read_handle, write_handle};
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_PIPE_HPP__
