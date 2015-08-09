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

#include <stout/strings.hpp>

/**
 * Represents a POSIX file systems path and offers common path
 * manipulations.
 */
class Path
{
public:
  explicit Path(const std::string& path)
    : value(strings::remove(path, "file://", strings::PREFIX)) {}

  // TODO(cmaloney): Add more useful operations such as 'absolute()',
  // 'directoryname()', 'filename()', etc.

  /**
   * Extracts the component following the final '/'. Trailing '/'
   * characters are not counted as part of the pathname.
   *
   * Like the standard '::basename()' except it is thread safe.
   *
   * The following list of examples (taken from SUSv2) shows the
   * strings returned by basename() for different paths:
   *
   * path        | basename
   * ----------- | -----------
   * "/usr/lib"  | "lib"
   * "/usr/"     | "usr"
   * "usr"       | "usr"
   * "/"         | "/"
   * "."         | "."
   * ".."        | ".."
   *
   * @return The component following the final '/'. If Path does not
   *   contain a '/', this returns a copy of Path. If Path is the
   *   string "/", then this returns the string "/". If Path is an
   *   empty string, then it returns the string ".".
   */
  inline std::string basename() const
  {
    if (value.empty()) {
      return std::string(".");
    }

    size_t end = value.size() - 1;

    // Remove trailing slashes.
    if (value[end] == '/') {
      end = value.find_last_not_of('/', end);

      // Paths containing only slashes result into "/".
      if (end == std::string::npos) {
        return std::string("/");
      }
    }

    // 'start' should point towards the character after the last slash
    // that is non trailing.
    size_t start = value.find_last_of('/', end);

    if (start == std::string::npos) {
      start = 0;
    } else {
      start++;
    }

    return value.substr(start, end + 1 - start);
  }

  /**
   * Extracts the component up to, but not including, the final '/'.
   * Trailing '/' characters are not counted as part of the pathname.
   *
   * Like the standard '::dirname()' except it is thread safe.
   *
   * The following list of examples (taken from SUSv2) shows the
   * strings returned by dirname() for different paths:
   *
   * path        | dirname
   * ----------- | -----------
   * "/usr/lib"  | "/usr"
   * "/usr/"     | "/"
   * "usr"       | "."
   * "/"         | "/"
   * "."         | "."
   * ".."        | "."
   *
   * @return The component up to, but not including, the final '/'. If
   *   Path does not contain a '/', then this returns the string ".".
   *   If Path is the string "/", then this returns the string "/".
   *   If Path is an empty string, then this returns the string ".".
   */
  inline std::string dirname() const
  {
    if (value.empty()) {
      return std::string(".");
    }

    size_t end = value.size() - 1;

    // Remove trailing slashes.
    if (value[end] == '/') {
      end = value.find_last_not_of('/', end);
    }

    // Remove anything trailing the last slash.
    end = value.find_last_of('/', end);

    // Paths containing no slashes result in ".".
    if (end == std::string::npos) {
      return std::string(".");
    }

    // Paths containing only slashes result in "/".
    if (end == 0) {
      return std::string("/");
    }

    // 'end' should point towards the last non slash character
    // preceding the last slash.
    end = value.find_last_not_of('/', end);

    // Paths containing no non slash characters result in "/".
    if (end == std::string::npos) {
      return std::string("/");
    }

    return value.substr(0, end + 1);
  }

  // Implicit conversion from Path to string.
  operator std::string() const
  {
    return value;
  }

  const std::string value;
};


inline std::ostream& operator<<(
    std::ostream& stream,
    const Path& path)
{
  return stream << path.value;
}


namespace path {

// Base case.
inline std::string join(const std::string& path1, const std::string& path2)
{
  return strings::remove(path1, "/", strings::SUFFIX) + "/" +
         strings::remove(path2, "/", strings::PREFIX);
}


template <typename... Paths>
inline std::string join(
    const std::string& path1,
    const std::string& path2,
    Paths&&... paths)
{
  return join(path1, join(path2, std::forward<Paths>(paths)...));
}


inline std::string join(const std::vector<std::string>& paths)
{
  if (paths.empty()) {
    return "";
  }

  std::string result = paths[0];
  for (size_t i = 1; i < paths.size(); ++i) {
    result = join(result, paths[i]);
  }
  return result;
}

} // namespace path {

#endif // __STOUT_PATH_HPP__
