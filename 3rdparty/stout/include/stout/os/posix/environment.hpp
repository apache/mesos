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

#ifndef __STOUT_OS_POSIX_ENVIRONMENT_HPP__
#define __STOUT_OS_POSIX_ENVIRONMENT_HPP__

#include <map>
#include <string>

#include <stout/os/raw/environment.hpp>


namespace os {

inline std::map<std::string, std::string> environment()
{
  char** env = os::raw::environment();

  std::map<std::string, std::string> result;

  for (size_t index = 0; env[index] != nullptr; index++) {
    std::string entry(env[index]);
    size_t position = entry.find_first_of('=');
    if (position == std::string::npos) {
      continue; // Skip malformed environment entries.
    }

    result[entry.substr(0, position)] = entry.substr(position + 1);
  }

  return result;
}

} // namespace os {

#endif // __STOUT_OS_POSIX_ENVIRONMENT_HPP__
