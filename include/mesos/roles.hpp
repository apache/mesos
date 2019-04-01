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

#ifndef __ROLES_HPP__
#define __ROLES_HPP__

#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace roles {

/**
 * Returns true iff `left` is a strict subrole of `right`. `left` is a strict
 * subrole of `right` if `left` is not equal to `right`, and `left` is a
 * descendant of `right` in the role hierarchy.
 *
 * Examples:
 *   - `foo` is not a strict subrole of `foo`.
 *   - `foo/bar` is a strict subrole of `foo`.
 *   - `foobar` is not a strict subrole of `foo`.
 */
bool isStrictSubroleOf(const std::string& left, const std::string& right);


/**
 * Parses Roles from text in the form "role1,role2,role3".
 *
 * @param text String to be parsed
 * @return Error if validation fails, otherwise a list of role names.
 */
Try<std::vector<std::string>> parse(const std::string& text);


/**
 * Returns the ancestor roles for the role.
 * E.g. "a/b/c/d" returns ["a/b/c", "a/b", "a"].
 *
 * Assumes the provided role is valid.
 *
 * TODO(bmahler): Use string_view to avoid copying, this requires
 * overloading to ensure that the argument is not an rvalue.
 */
std::vector<std::string> ancestors(const std::string& role);


/**
 * Validates the given role name. A role name must be a valid directory name,
 * so it cannot:
 * - Be an empty string
 * - Be `.` or `..`
 * - Start with `-`
 * - Contain invalid characters (slash, backspace, or whitespace).
 *
 * @param role Role name to be validated
 * @return Error if validation fails for any role, None otherwise.
 */
Option<Error> validate(const std::string& role);


/**
 * Validates the given list of roles. Returns Error if any role is invalid.
 *
 * @param roles List of role names to be validated
 * @return Error if validation fails, None otherwise.
 */
Option<Error> validate(const std::vector<std::string>& roles);

} // namespace roles {
} // namespace mesos {

#endif // __ROLES_HPP__
