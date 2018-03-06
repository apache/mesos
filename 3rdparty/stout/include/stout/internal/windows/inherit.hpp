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

#ifndef __STOUT_INTERNAL_WINDOWS_INHERIT_HPP__
#define __STOUT_INTERNAL_WINDOWS_INHERIT_HPP__

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include <stout/os/windows/fd.hpp>
#include <stout/windows.hpp>


namespace internal {
namespace windows {

// This function enables or disables inheritance for a Windows file handle.
//
// NOTE: By default, handles on Windows are not inheritable, so this is
// primarily used to enable inheritance when passing handles to child processes,
// and subsequently disable inheritance.
inline Try<Nothing> set_inherit(const os::WindowsFD& fd, const bool inherit)
{
  const BOOL result = ::SetHandleInformation(
      fd, HANDLE_FLAG_INHERIT, inherit ? HANDLE_FLAG_INHERIT : 0);

  if (result == FALSE) {
    return WindowsError();
  }

  return Nothing();
}

} // namespace windows {
} // namespace internal {

#endif // __STOUT_INTERNAL_WINDOWS_INHERIT_HPP__
