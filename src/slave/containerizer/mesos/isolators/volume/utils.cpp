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

#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "slave/containerizer/mesos/isolators/volume/utils.hpp"

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace volume {

PathValidator::PathValidator(const vector<string>& _whitelist)
  : whitelist(_whitelist) {}


PathValidator PathValidator::parse(const string& whitelist)
{
  return PathValidator(strings::split(whitelist, HOST_PATH_WHITELIST_DELIM));
}


Try<Nothing> PathValidator::validate(const string& path) const
{
  foreach (const string& allowedPath, whitelist) {
    const string allowedDirectory = path::join(
        allowedPath, stringify(os::PATH_SEPARATOR));
    if (path == allowedPath || strings::startsWith(path, allowedDirectory)) {
      return Nothing();
    }
  }

  return Error("Path '" + path + "' is not whitelisted");
}

} // namespace volume {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
