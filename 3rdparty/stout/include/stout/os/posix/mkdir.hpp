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

#ifndef __STOUT_OS_POSIX_MKDIR_HPP__
#define __STOUT_OS_POSIX_MKDIR_HPP__

#include <sys/stat.h>

#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/nothing.hpp>
#include <stout/strings.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/fsync.hpp>

namespace os {

// Make a directory.
//
// If `recursive` is set to true, all intermediate directories will be created
// as required. If `sync` is set to true, `fsync()` will be called on the parent
// of each created directory to ensure that the result is committed to its
// filesystem.
//
// NOTE: This function doesn't ensure that any existing directory is committed
// to its filesystem, and it does not perform any cleanup in case of a failure.
inline Try<Nothing> mkdir(
    const std::string& directory,
    bool recursive = true,
    bool sync = false)
{
  if (!recursive) {
    if (::mkdir(directory.c_str(), 0755) < 0) {
      return ErrnoError();
    }

    if (sync) {
      const std::string parent = Path(directory).dirname();
      Try<Nothing> fsync = os::fsync(parent);
      if (fsync.isError()) {
        return Error(
            "Failed to fsync directory '" + parent + "': " + fsync.error());
      }
    }
  } else {
    std::vector<std::string> tokens =
      strings::tokenize(directory, stringify(os::PATH_SEPARATOR));

    std::string path;

    // We got an absolute path, so keep the leading slash.
    if (directory.find_first_of(stringify(os::PATH_SEPARATOR)) == 0) {
      path = os::PATH_SEPARATOR;
    }

    foreach (const std::string& token, tokens) {
      path += token;
      if (::mkdir(path.c_str(), 0755) < 0) {
        if (errno != EEXIST) {
          return ErrnoError();
        }
      } else if (sync) {
        const std::string parent = Path(path).dirname();
        Try<Nothing> fsync = os::fsync(parent);
        if (fsync.isError()) {
          return Error(
              "Failed to fsync directory '" + parent + "': " + fsync.error());
        }
      }

      path += os::PATH_SEPARATOR;
    }
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_POSIX_MKDIR_HPP__
