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

#include <string>

#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/version.hpp>

#include <common/kernel_version.hpp>

using std::string;
using std::vector;

namespace mesos {

Try<Version> kernelVersion()
{
  Try<os::UTSInfo> uname = os::uname();
  if (!uname.isSome()) {
    return Error("Unable to determine kernel version: " + uname.error());
  }

  vector<string> parts = strings::split(uname->release, ".");
  parts.resize(2);

  Try<Version> version = Version::parse(strings::join(".", parts));
  if (!version.isSome()) {
    return Error("Failed to parse kernel version '" + uname->release +
        "': " + version.error());
  }

  return version;
}

} // namespace mesos {
