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

#ifndef __STOUT_FLAGS_PARSE_HPP__
#define __STOUT_FLAGS_PARSE_HPP__

#include <sstream> // For istringstream.
#include <string>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/json.hpp>
#include <stout/path.hpp>
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

  if (in && in.eof()) {
    return t;
  }

  return Error("Failed to convert into required type");
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
  // A value that already starts with 'file://' will properly be
  // loaded from the file and put into 'value' but if it starts with
  // '/' we need to explicitly handle it for backwards compatibility
  // reasons (because we used to handle it before we introduced the
  // 'fetch' mechanism for flags that first fetches the data from URIs
  // such as 'file://').
  if (strings::startsWith(value, "/")) {
    LOG(WARNING) << "Specifying an absolute filename to read a command line "
                    "option out of without using 'file:// is deprecated and "
                    "will be removed in a future release. Simply adding "
                    "'file://' to the beginning of the path should eliminate "
                    "this warning.";

    Try<std::string> read = os::read(value);
    if (read.isError()) {
      return Error("Error reading file '" + value + "': " + read.error());
    }
    return JSON::parse<JSON::Object>(read.get());
  }
  return JSON::parse<JSON::Object>(value);
}


template <>
inline Try<JSON::Array> parse(const std::string& value)
{
  // A value that already starts with 'file://' will properly be
  // loaded from the file and put into 'value' but if it starts with
  // '/' we need to explicitly handle it for backwards compatibility
  // reasons (because we used to handle it before we introduced the
  // 'fetch' mechanism for flags that first fetches the data from URIs
  // such as 'file://').
  if (strings::startsWith(value, "/")) {
    LOG(WARNING) << "Specifying an absolute filename to read a command line "
                    "option out of without using 'file:// is deprecated and "
                    "will be removed in a future release. Simply adding "
                    "'file://' to the beginning of the path should eliminate "
                    "this warning.";

    Try<std::string> read = os::read(value);
    if (read.isError()) {
      return Error("Error reading file '" + value + "': " + read.error());
    }
    return JSON::parse<JSON::Array>(read.get());
  }
  return JSON::parse<JSON::Array>(value);
}


template <>
inline Try<Path> parse(const std::string& value)
{
  return Path(value);
}

} // namespace flags {

#endif // __STOUT_FLAGS_PARSE_HPP__
