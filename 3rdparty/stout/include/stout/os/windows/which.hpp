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

#ifndef __STOUT_OS_WINDOWS_WHICH_HPP__
#define __STOUT_OS_WINDOWS_WHICH_HPP__

#include <string>
#include <vector>

#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>


namespace os {

// This behaves "like the user expects" of POSIX `which`, but on Windows. That
// is, if a path is not specified, we search through the `PATH` environment
// variable, but explicitly do not search the current working directory (which
// `CreateProcess` and Windows' `where` would do).
//
// Because the executable permission does not work on Windows, the closest
// equivalent is to check the path extension against those listed in `PATHEXT`.
// However, we first search for exactly the file name the user specified with
// `command`, regardless of extension, because it could be an executable. If an
// exact match is not found, we continue the search with the environment's
// "executable" extensions.
inline Option<std::string> which(
    const std::string& command,
    const Option<std::string>& _path = None())
{
  Option<std::string> path = _path;

  if (path.isNone()) {
    path = os::getenv("PATH");

    if (path.isNone()) {
      return None();
    }
  }

  Option<std::string> pathext = os::getenv("PATHEXT");

  if (pathext.isNone()) {
    pathext = ".COM;.EXE;.BAT;.CMD";
  }

  std::vector<std::string> tokens = strings::tokenize(path.get(), ";");
  std::vector<std::string> exts = strings::tokenize(pathext.get(), ";");

  // NOTE: This handles the edge case of `command` already having an extension,
  // e.g. `docker.exe`. By starting with the case of "", we don't have to
  // special case the loops below.
  exts.insert(exts.begin(), "");

  // Nested loops, but fairly finite. This is how `where` works on Windows
  // (which is the equivalent of `which`). The loops are nested such that we
  // first search through `PATH` for `command`, then through `PATH` for
  // `command.COM` and so on.
  foreach (const std::string& ext, exts) {
    foreach (const std::string& token, tokens) {
      const std::string commandPath = path::join(token, command + ext);
      if (!os::exists(commandPath)) {
        continue;
      }

      return commandPath;
    }
  }

  return None();
}

} // namespace os {


#endif // __STOUT_OS_WINDOWS_WHICH_HPP__
