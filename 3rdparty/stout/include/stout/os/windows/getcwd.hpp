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

#ifndef __STOUT_OS_WINDOWS_GETCWD_HPP__
#define __STOUT_OS_WINDOWS_GETCWD_HPP__

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/internal/windows/longpath.hpp>


namespace os {

// TODO(josephw): Consider changing the return type to a `Try<std::string>`
// so that we do not CHECK-fail upon error.
inline std::string getcwd()
{
  // First query for the buffer size required.
  const DWORD length = ::GetCurrentDirectoryW(0, nullptr);
  CHECK(length != 0) << "Failed to retrieve current directory buffer size";

  std::vector<wchar_t> buffer;
  buffer.reserve(static_cast<size_t>(length));

  const DWORD result = ::GetCurrentDirectoryW(length, buffer.data());
  CHECK(result != 0) << "Failed to determine current directory";

  return strings::remove(
      stringify(std::wstring(buffer.data())),
      os::LONGPATH_PREFIX,
      strings::Mode::PREFIX);
}

} // namespace os {


#endif // __STOUT_OS_WINDOWS_GETCWD_HPP__
