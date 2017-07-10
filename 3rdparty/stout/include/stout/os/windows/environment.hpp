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

#ifndef __STOUT_OS_WINDOWS_ENVIRONMENT_HPP__
#define __STOUT_OS_WINDOWS_ENVIRONMENT_HPP__

#include <map>
#include <string>
#include <stout/stringify.hpp>


namespace os {

inline std::map<std::string, std::string> environment()
{
  // NOTE: The `GetEnvironmentStrings` call returns a pointer to a
  // read-only block of memory with the following format (minus newlines):
  // Var1=Value1\0
  // Var2=Value2\0
  // Var3=Value3\0
  // ...
  // VarN=ValueN\0\0
  wchar_t* env = ::GetEnvironmentStringsW();
  std::map<std::string, std::string> result;

  for (size_t i = 0; env[i] != L'\0' && env[i+1] != L'\0';) {
    std::wstring entry(env + i);

    // Increment past the current environment string and null terminator.
    i = i + entry.size() + 1;

    size_t position = entry.find_first_of(L'=');
    if (position == std::string::npos) {
      continue; // Skip malformed environment entries.
    }

    result[stringify(entry.substr(0, position))] =
      stringify(entry.substr(position + 1));
  }

  return result;
}

} // namespace os {

#endif // __STOUT_OS_WINDOWS_ENVIRONMENT_HPP__
