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

#ifndef __STOUT_OS_MKDIR_HPP__
#define __STOUT_OS_MKDIR_HPP__

#ifndef __WINDOWS__
#include <sys/stat.h>
#endif // __WINDOWS__

#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/constants.hpp>

#ifdef __WINDOWS__
#include <stout/windows.hpp> // To be certain we're using the right `mkdir`.
#endif // __WINDOWS__


namespace os {

inline Try<Nothing> mkdir(const std::string& directory, bool recursive = true)
{
  if (!recursive) {
    if (::mkdir(directory.c_str(), 0755) < 0) {
      return ErrnoError();
    }
  } else {
    std::vector<std::string> tokens =
      strings::tokenize(directory, stringify(os::PATH_SEPARATOR));

    std::string path = "";

    // We got an absolute path, so keep the leading slash.
    if (directory.find_first_of(stringify(os::PATH_SEPARATOR)) == 0) {
      path = os::PATH_SEPARATOR;
    }

    foreach (const std::string& token, tokens) {
      path += token;
      if (::mkdir(path.c_str(), 0755) < 0 && errno != EEXIST) {
        return ErrnoError();
      }

      path += os::PATH_SEPARATOR;
    }
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_MKDIR_HPP__
