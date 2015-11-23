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

#include "linux/systemd.hpp"

#include <string>
#include <vector>

#include <process/once.hpp>

#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

using process::Once;

using std::string;
using std::vector;

namespace systemd {

int DELEGATE_MINIMUM_VERSION = 218;


Flags::Flags()
{
  add(&Flags::runtime_directory,
      "runtime_directory",
      "The path to the systemd system run time directory\n",
      "/run/systemd/system");

  add(&Flags::cgroups_hierarchy,
      "cgroups_hierarchy",
      "The path to the cgroups hierarchy root\n",
      "/sys/fs/cgroup");
}


static Flags* systemd_flags = NULL;


const Flags& flags()
{
  return *CHECK_NOTNULL(systemd_flags);
}


Try<Nothing> initialize(const Flags& flags)
{
  static Once* initialized = new Once();

  if (initialized->once()) {
    return Nothing();
  }

  systemd_flags = new Flags(flags);

  // If flags->runtime_directory doesn't exist, then we can't proceed.
  if (!os::exists(CHECK_NOTNULL(systemd_flags)->runtime_directory)) {
    return Error("Failed to locate systemd runtime directory: " +
                 CHECK_NOTNULL(systemd_flags)->runtime_directory);
  }

  initialized->done();

  return Nothing();
}


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


Path runtimeDirectory()
{
  return Path(flags().runtime_directory);
}


Path hierarchy()
{
  return Path(path::join(flags().cgroups_hierarchy, "systemd"));
}


Try<Nothing> daemonReload()
{
  Try<std::string> daemonReload = os::shell("systemctl daemon-reload");
  if (daemonReload.isError()) {
    return Error("Failed to reload systemd daemon: " + daemonReload.error());
  }

  return Nothing();
}

namespace slices {

bool exists(const Path& path)
{
  return os::exists(path);
}


Try<Nothing> create(const Path& path, const string& data)
{
  Try<Nothing> write = os::write(path, data);
  if (write.isError()) {
    return Error(
        "Failed to write systemd slice `" + path.value + "`: " + write.error());
  }

  LOG(INFO) << "Created systemd slice: `" << path << "`";

  Try<Nothing> reload = daemonReload();
  if (reload.isError()) {
    return Error("Failed to create systemd slice `" + path.value + "`: " +
                 reload.error());
  }

  return Nothing();
}


Try<Nothing> start(const std::string& name)
{
  Try<std::string> start = os::shell("systemctl start " + name);

  if (start.isError()) {
    return Error(
        "Failed to start systemd slice `" + name + "`: " + start.error());
  }

  LOG(INFO) << "Started systemd slice `" << name << "`";

  return Nothing();
}

} // namespace slices {

} // namespace systemd {
