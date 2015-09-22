/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "linux/systemd.hpp"

#include <string>
#include <vector>

#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

using std::string;
using std::vector;

namespace systemd {

int DELEGATE_MINIMUM_VERSION = 218;

bool exists()
{
  // This is static as the init system should not change while we are running.
  static const bool exists = []() -> bool {
    // (1) Test whether `/sbin/init` links to systemd.
    const Result<string> realpath = os::realpath("/sbin/init");
    if (realpath.isError() || realpath.isNone()) {
      LOG(WARNING) << "Failed to test /sbin/init for systemd environment: "
                   << realpath.error();

      return false;
    }

    CHECK_SOME(realpath);

    // (2) Testing whether we have a systemd version.
    const string command = realpath.get() + " --version";
    Try<string> versionCommand = os::shell(command);

    if (versionCommand.isError()) {
      LOG(WARNING) << "Failed to test command '" << command << "': "
                   << versionCommand.error();

      return false;
    }

    vector<string> tokens = strings::tokenize(versionCommand.get(), " \n");

    // We need at least a name and a version number to match systemd.
    if (tokens.size() < 2) {
      return false;
    }

    if (tokens[0] != "systemd") {
      return false;
    }

    Try<int> version = numify<int>(tokens[1]);
    if (version.isError()) {
      LOG(WARNING) << "Failed to parse systemd version '" << tokens[1] << "'";
      return false;
    }

    LOG(INFO) << "systemd version `" << version.get() << "` detected";

    // We log a warning if the version is below 218. This is because the
    // `Delegate` flag was introduced in version 218. Some systems, like RHEL 7,
    // have patched versions that are below 218 but still have the `Delegate`
    // flag. This is why we warn / inform users rather than failing. See
    // MESOS-3352.
    if (version.get() < DELEGATE_MINIMUM_VERSION) {
      LOG(WARNING)
        << "Required functionality `Delegate` was introduced in Version `"
        << DELEGATE_MINIMUM_VERSION << "`. Your system may not function"
        << " properly; however since some distributions have patched systemd"
        << " packages, your system may still be functional. This is why we keep"
        << " running. See MESOS-3352 for more information";
    }

    return true;
  }();

  return exists;
}

} // namespace systemd {
