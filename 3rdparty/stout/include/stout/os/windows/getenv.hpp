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

#ifndef __STOUT_OS_WINDOWS_GETENV_HPP__
#define __STOUT_OS_WINDOWS_GETENV_HPP__

#include <memory>
#include <string>

#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/windows.hpp>


namespace os {

// Looks in the environment variables for the specified key and
// returns a string representation of its value. If no environment
// variable matching key is found, None() is returned.
inline Option<std::string> getenv(const std::string& key)
{
  std::wstring wide_key = wide_stringify(key);

  // NOTE: The double-call to `::GetEnvironmentVariable` here uses the first
  // call to get the size of the variable's value, and then again to retrieve
  // the value itself. It is possible to have `::GetEnvironmentVariable`
  // allocate the space for this, but we explicitly do it this way to avoid
  // that.
  const DWORD buffer_size =
    ::GetEnvironmentVariableW(wide_key.data(), nullptr, 0);
  if (buffer_size == 0) {
    if (::GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
      return None();
    }

    return "";
  }

  std::vector<wchar_t> environment;
  environment.reserve(static_cast<size_t>(buffer_size));

  DWORD value_size =
    ::GetEnvironmentVariableW(wide_key.data(), environment.data(), buffer_size);

  if (value_size == 0) {
    // If `value_size == 0` here, that probably means the environment variable
    // was deleted between when we checked and when we allocated the buffer.
    if (::GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
      return None();
    }

    return "";
  }

  return stringify(std::wstring(environment.data()));
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_GETENV_HPP__
