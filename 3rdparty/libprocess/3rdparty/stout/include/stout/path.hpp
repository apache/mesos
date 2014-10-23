/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_PATH_HPP__
#define __STOUT_PATH_HPP__

#include <string>
#include <utility>
#include <vector>

#include "strings.hpp"

namespace path {


template<typename ...T>
std::string join(const std::string& path, T&&... tail)
{
  std::string tailJoined = strings::join(
      "/",
      strings::trim(std::forward<T>(tail), "/")...);

  // The first path chunk is special in that if it starts with a '/',
  // we want to keep that.
  if (path.empty()) {
    return tailJoined;
  }

  // If the first chunk ends with a '/', don't append another using
  // join. This also handles the case with the first path part is just
  // '/'.
  if (path.back() == '/') {
    return path + tailJoined;
  }
  return strings::join("/", path, tailJoined);
}


inline std::string join(const std::vector<std::string>& paths)
{
  if (paths.empty()) {
    return "";
  }

  std::string result = paths[0];
  for (size_t i = 1; i < paths.size(); ++i) {
    const std::string &path = paths[i];

    // Don't insert extra '/' for empty paths.
    if (path.empty()) {
      continue;
    }

    // If result is empty, fill it.
    if (result.empty()) {
      result = path;
      continue;
    }
    result = join(result, path);
  }
  return result;
}

} // namespace path {

#endif // __STOUT_PATH_HPP__
