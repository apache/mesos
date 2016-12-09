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

#ifndef __STOUT_OS_POSIX_GETENV_HPP__
#define __STOUT_OS_POSIX_GETENV_HPP__

#include <stdlib.h>

#include <string>

#include <stout/none.hpp>
#include <stout/option.hpp>


namespace os {

// Looks in the environment variables for the specified key and
// returns a string representation of its value. If no environment
// variable matching key is found, None() is returned.
inline Option<std::string> getenv(const std::string& key)
{
  char* value = ::getenv(key.c_str());

  if (value == nullptr) {
    return None();
  }

  return std::string(value);
}

} // namespace os {

#endif // __STOUT_OS_POSIX_GETENV_HPP__
