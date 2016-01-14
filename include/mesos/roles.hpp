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

#include <stout/try.hpp>

namespace mesos {
namespace roles {

/**
 * Parses Roles from text in the form "role1,role2,role3".
 *
 * @param text String to be parsed
 * @return Error if validation fails, otherwise a list of role names.
 */
Try<std::vector<std::string>> parse(const std::string& text);


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
