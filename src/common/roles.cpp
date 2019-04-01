// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <mesos/roles.hpp>

#include <vector>
#include <string>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/option.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

using std::string;
using std::vector;

namespace mesos {
namespace roles {

bool isStrictSubroleOf(const std::string& left, const std::string& right)
{
  return left.size() > right.size() &&
         left[right.size()] == '/' &&
         strings::startsWith(left, right);
}


// TODO(haosdent): Remove this function after we stop supporting `--roles`
// flag in master.
Try<vector<string>> parse(const string& text)
{
  vector<string> roles = strings::tokenize(text, ",");

  Option<Error> error = validate(roles);
  if (error.isSome()) {
    return error.get();
  } else {
    return roles;
  }
}


vector<string> ancestors(const string& role)
{
  vector<string> result;

  for (int i = role.size() - 1; i >= 0; --i) {
    if (role[i] == '/') {
      result.push_back(role.substr(0, i));
    }
  }

  return result;
}


// TODO(haosdent): Pull this out into `stout` and make it satisfy all
// OS/locale constraints.
// \x09 is horizontal tab (whitespace);
// \x0a is line feed (whitespace);
// \x0b is vertical tab (whitespace);
// \x0c is form feed (whitespace);
// \x0d is carriage return (whitespace);
// \x20 is space (whitespace);
// \x7f is backspace (del);
static const string* INVALID_CHARACTERS =
  new string("\x09\x0a\x0b\x0c\x0d\x20\x7f");


Option<Error> validate(const string& role)
{
  // We check * explicitly first as a performance improvement.
  static const string* star = new string("*");
  if (role == *star) {
    return None();
  }

  if (strings::startsWith(role, '/')) {
    return Error("Role '" + role + "' cannot start with a slash");
  }

  if (strings::endsWith(role, '/')) {
    return Error("Role '" + role + "' cannot end with a slash");
  }

  if (strings::contains(role, "//")) {
    return Error("Role '" + role + "' cannot contain two adjacent slashes");
  }

  // Validate each component in the role path.
  vector<string> components = strings::tokenize(role, "/");
  if (components.empty()) {
    return Error("Role names cannot be the empty string");
  }

  static const string* dot = new string(".");
  static const string* dotdot = new string("..");

  foreach (const string& component, components) {
    CHECK(!component.empty()); // `tokenize` does not return empty tokens.

    if (component == *dot) {
      return Error("Role '" + role + "' cannot include '.' as a component");
    }

    if (component == *dotdot) {
      return Error("Role '" + role + "' cannot include '..' as a component");
    }

    if (component == *star) {
      return Error("Role '" + role + "' cannot include '*' as a component");
    }

    if (strings::startsWith(component, '-')) {
      return Error("Role component '" + component + "' is invalid "
                   "because it starts with a dash");
    }

    if (component.find_first_of(*INVALID_CHARACTERS) != string::npos) {
      return Error("Role component '" + component + "' is invalid "
                   "because it contains backspace or whitespace");
    }
  }

  return None();
}


Option<Error> validate(const vector<string>& roles)
{
  foreach (const string& role, roles) {
    Option<Error> error = validate(role);
    if (error.isSome()) {
      return error.get();
    }
  }

  return None();
}

} // namespace roles {
} // namespace mesos {
