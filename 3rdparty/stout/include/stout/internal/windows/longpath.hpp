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

#ifndef __STOUT_INTERNAL_WINDOWS_LONGPATH_HPP__
#define __STOUT_INTERNAL_WINDOWS_LONGPATH_HPP__

#include <string>

#include <assert.h>

#include <stout/path.hpp>
#include <stout/stringify.hpp>

#include <stout/os/constants.hpp>


namespace internal {
namespace windows {

// This function idempotently prepends "\\?\" to the given path iff:
// (1) The path's length is greater than 248, the minimum Windows API limit.
//     This limit is neither `NAME_MAX` nor `PATH_MAX`; it is an arbitrary
//     limit of `CreateDirectory` and is the smallest such limit.
// (2) The path is absolute (otherwise the marker is meaningless).
// (3) The path does not already have the marker (idempotent).
//
// It then converts the path to UTF-16, appropriate for use in Uniode versions
// of Windows filesystem APIs which support lengths greater than NAME_MAX.
inline std::wstring longpath(const std::string& path)
{
  const size_t max_path_length = 248;
  if (path.size() > max_path_length &&
      path::absolute(path) &&
      !strings::startsWith(path, os::LONGPATH_PREFIX)) {
    return wide_stringify(os::LONGPATH_PREFIX + path);
  } else {
    return wide_stringify(path);
  }
}

} // namespace windows {
} // namespace internal {

#endif // __STOUT_INTERNAL_WINDOWS_LONGPATH_HPP__
