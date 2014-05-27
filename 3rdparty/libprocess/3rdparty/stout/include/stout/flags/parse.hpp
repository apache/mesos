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
#ifndef __STOUT_FLAGS_PARSE_HPP__
#define __STOUT_FLAGS_PARSE_HPP__

#include <sstream> // For istringstream.
#include <string>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/json.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/read.hpp>

namespace flags {

template <typename T>
Try<T> parse(const std::string& value)
{
  T t;
  std::istringstream in(value);
  in >> t;
  if (!in.good() && !in.eof()) {
    return Error("Failed to convert into required type");
  }
  return t;
}


template <>
inline Try<std::string> parse(const std::string& value)
{
  return value;
}


template <>
inline Try<bool> parse(const std::string& value)
{
  if (value == "true" || value == "1") {
    return true;
  } else if (value == "false" || value == "0") {
    return false;
  }
  return Error("Expecting a boolean (e.g., true or false)");
}


template <>
inline Try<Duration> parse(const std::string& value)
{
  return Duration::parse(value);
}


template <>
inline Try<Bytes> parse(const std::string& value)
{
  return Bytes::parse(value);
}


template <>
inline Try<JSON::Object> parse(const std::string& value)
{
  // If the flag value corresponds to a file parse the contents of the
  // file as JSON.
  // TODO(vinod): We do not support relative paths because it is
  // tricky to figure out if a flag value corresponds to a relative
  // path or a JSON string. For example, "{", "  {" and "  \n {" are
  // all valid prefixes of a JSON string.
  if (strings::startsWith(value, "/") ||
      strings::startsWith(value, "file://")) {
    const std::string& path =
      strings::remove(value, "file://", strings::PREFIX);

    Try<std::string> read = os::read(path);
    if (read.isError()) {
      return Error("Error reading file '" + path + "': " + read.error());
    }

    return JSON::parse<JSON::Object>(read.get());
  }

  return JSON::parse<JSON::Object>(value);
}

} // namespace flags {

#endif // __STOUT_FLAGS_PARSE_HPP__
