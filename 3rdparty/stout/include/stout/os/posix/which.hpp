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

#ifndef __STOUT_OS_POSIX_WHICH_HPP__
#define __STOUT_OS_POSIX_WHICH_HPP__

#include <string>
#include <vector>

#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/permissions.hpp>


namespace os {

inline Option<std::string> which(
    const std::string& command,
    const Option<std::string>& _path = None())
{
  Option<std::string> path = _path;

  if (path.isNone()) {
    path = getenv("PATH");

    if (path.isNone()) {
      return None();
    }
  }

  std::vector<std::string> tokens = strings::tokenize(path.get(), ":");
  foreach (const std::string& token, tokens) {
    const std::string commandPath = path::join(token, command);
    if (!os::exists(commandPath)) {
      continue;
    }

    Try<os::Permissions> permissions = os::permissions(commandPath);
    if (permissions.isError()) {
      continue;
    }

    if (!permissions->owner.x &&
        !permissions->group.x &&
        !permissions->others.x) {
      continue;
    }

    return commandPath;
  }

  return None();
}

} // namespace os {


#endif // __STOUT_OS_POSIX_WHICH_HPP__
