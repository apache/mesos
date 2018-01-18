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

#ifndef __STOUT_FLAGS_FLAG_HPP__
#define __STOUT_FLAGS_FLAG_HPP__

#include <ostream>
#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

namespace flags {

// Forward declaration.
class FlagsBase;


struct Name
{
  Name()
    : deprecated(false) {}

  Name(const std::string& _value)
    : value(_value), deprecated(false) {}

  Name(const char* _value)
    : value(_value), deprecated(false) {}

  bool operator==(const Name& other) const
  {
    return value == other.value;
  }

  std::string value;
  bool deprecated;
};


inline Name DeprecatedName(const std::string& name)
{
  Name name_(name);
  name_.deprecated = true;
  return name_;
}


// NOTE: Move this to `warning.hpp` if this can be used elsewhere.
struct Warning
{
public:
  explicit Warning(const std::string& _message) : message(_message) {}

  const std::string message;
};


// Convenience wrapper.
struct Warnings
{
  std::vector<Warning> warnings;
};


struct Flag
{
  Name name;
  Option<Name> alias;

  // This is the name that the user uses to specifically load the flag (e.g, via
  // command line `--foo=val`). This is optional because a flag might not be
  // explicitly loaded by the user (e.g., flag with a default value). Note that
  // this name should be one of `name` or `alias`.
  Option<Name> loaded_name;

  std::string help;
  bool boolean;
  lambda::function<Try<Nothing>(FlagsBase*, const std::string&)> load;
  lambda::function<Option<std::string>(const FlagsBase&)> stringify;
  lambda::function<Option<Error>(const FlagsBase&)> validate;
  bool required;

  // This is the name of the flag that the user loads. If the loading is
  // implicit this defaults to the `name`.
  const Name& effective_name() const
  {
    return loaded_name.isSome() ? loaded_name.get() : name;
  }
};


// Allows to hide the contents of a file located at `path` when serializing
// a flag. This is important for files containing sensitive information.
struct SecurePathOrValue
{
  Option<Path> path;
  std::string value;
};


inline std::ostream& operator<<(
    std::ostream &stream,
    const SecurePathOrValue& flag)
{
  if (flag.path.isSome()) {
    stream << flag.path.get();
  } else {
    stream << flag.value;
  }
  return stream;
}

} // namespace flags {

#endif // __STOUT_FLAGS_FLAG_HPP__
