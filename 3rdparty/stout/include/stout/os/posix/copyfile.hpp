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

#ifndef __STOUT_OS_POSIX_COPYFILE_HPP__
#define __STOUT_OS_POSIX_COPYFILE_HPP__

#include <string>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/os/exec.hpp>
#include <stout/os/stat.hpp>

namespace os {

// This implementation works by running the `cp` command with some
// additional conditions to ensure we copy a single file only,
// from an absolute file path to another absolute file path.
//
// Directories are not supported as a destination path for two reasons:
// 1. No callers depended on that behavior,
// 2. Consistency with Windows implementation.
//
// Relative paths are not allowed, as these are resolved based on
// the current working directory and may be inconsistent.
inline Try<Nothing> copyfile(
    const std::string& source, const std::string& destination)
{
  // NOTE: We check the form of the path too in case it does not exist, and to
  // prevent user error.
  if (stat::isdir(source) || source.back() == '/') {
    return Error("`source` was a directory");
  }

  if (stat::isdir(destination) || destination.back() == '/') {
    return Error("`destination` was a directory");
  }

  if (!path::is_absolute(source)) {
    return Error("`source` was a relative path");
  }

  if (!path::is_absolute(destination)) {
    return Error("`destination` was a relative path");
  }

  const Option<int> status = os::spawn("cp", {"cp", source, destination});

  if (status.isNone()) {
    return ErrnoError("os::spawn failed");
  }

  if (!(WIFEXITED(status.get()) && WEXITSTATUS(status.get()) == 0)) {
    return Error("cp failed with status: " + stringify(status.get()));
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_POSIX_COPYFILE_HPP__
