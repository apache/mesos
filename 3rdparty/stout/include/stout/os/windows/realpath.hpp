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

#ifndef __STOUT_OS_WINDOWS_REALPATH_HPP__
#define __STOUT_OS_WINDOWS_REALPATH_HPP__


#include <stout/error.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include <stout/internal/windows/longpath.hpp>


namespace os {

inline Result<std::string> realpath(const std::string& path)
{
  // TODO(andschwa): Test the existence of `path` to be consistent with POSIX
  // `::realpath`.

  std::wstring longpath = ::internal::windows::longpath(path);

  // First query for the buffer size required.
  DWORD length = GetFullPathNameW(longpath.data(), 0, nullptr, nullptr);
  if (length == 0) {
    return WindowsError("Failed to retrieve realpath buffer size");
  }

  std::vector<wchar_t> buffer;
  buffer.reserve(static_cast<size_t>(length));

  DWORD result =
    GetFullPathNameW(longpath.data(), length, buffer.data(), nullptr);

  if (result == 0) {
    return WindowsError("Failed to determine realpath");
  }

  return strings::remove(
      stringify(std::wstring(buffer.data())),
      os::LONGPATH_PREFIX,
      strings::Mode::PREFIX);
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_REALPATH_HPP__
