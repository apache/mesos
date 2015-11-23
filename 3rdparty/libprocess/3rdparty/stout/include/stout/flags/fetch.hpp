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

#ifndef __STOUT_FLAGS_FETCH_HPP__
#define __STOUT_FLAGS_FETCH_HPP__

#include <sstream> // For istringstream.
#include <string>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/json.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/flags/parse.hpp>

#include <stout/os/read.hpp>

namespace flags {

// Allow the value for a flag to be fetched/resolved from a URL such
// as file://foo/bar/baz. Use template specialization to add a custom
// fetcher for a given type, such as Path from <stout/path.hpp> Path
// has a custom fetcher so that it returns the path given rather than
// the value read from that path.
template <typename T>
Try<T> fetch(const std::string& value)
{
  // If the flag value corresponds to a file indicated by file://
  // fetch and then parse the contents of that file.
  //
  // TODO(cmaloney): Introduce fetching for things beyond just file://
  // such as http:// as well!
  if (strings::startsWith(value, "file://")) {
    const std::string path = value.substr(7);

    Try<std::string> read = os::read(path);
    if (read.isError()) {
      return Error("Error reading file '" + path + "': " + read.error());
    }

    return parse<T>(read.get());
  }

  return parse<T>(value);
}


template <>
inline Try<Path> fetch(const std::string& value)
{
  // Explicitly skip fetching the file if this is for a Path flag!
  return parse<Path>(value);
}

} // namespace flags {

#endif // __STOUT_FLAGS_FETCH_HPP__
