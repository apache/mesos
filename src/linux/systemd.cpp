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

#include <algorithm>
#include <string>
#include <vector>

#include <process/once.hpp>

#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/os/realpath.hpp>

#include <stout/flags/parse.hpp>

#include "linux/cgroups.hpp"

using process::Once;

using std::string;
using std::vector;

namespace systemd {

int DELEGATE_MINIMUM_VERSION = 218;

Flags::Flags()
{
  add(&Flags::enabled,
      "enabled",
      "Top level control of systemd support. When enabled, features such as\n"
      "processes life-time extension are enabled unless there is an explicit\n"
      "flag to disable these (see other flags).",
      true);

  add(&Flags::runtime_directory,
      "runtime_directory",
      "The path to the systemd system run time directory\n",
      "/run/systemd/system");

  add(&Flags::cgroups_hierarchy,
      "cgroups_hierarchy",
      "The path to the cgroups hierarchy root\n",
      "/sys/fs/cgroup");
}


static Flags* systemd_flags = nullptr;


const Flags& flags()
{
  return *CHECK_NOTNULL(systemd_flags);
}


namespace mesos {

// NOTE: Returning an Error implies the child process will be killed.
Try<Nothing> extendLifetime(pid_t child)
{
  if (!systemd::exists()) {
    return Error("Failed to contain process on systemd: "
                 "systemd does not exist on this system");
  }

  if (!systemd::enabled()) {
    return Error("Failed to contain process on systemd: "
                 "systemd is not configured as enabled on this system");
  }

  Try<Nothing> assign = cgroups::assign(
      hierarchy(),
      systemd::mesos::MESOS_EXECUTORS_SLICE,
      child);

  if (assign.isError()) {
    return Error("Failed to contain process on systemd: "
                 "Failed to assign process to its systemd executor slice: " +
                  assign.error());
  }

  LOG(INFO) << "Assigned child process '" << child << "' to '"
            << systemd::mesos::MESOS_EXECUTORS_SLICE << "'";

  return Nothing();
}

} // namespace mesos {


Try<Nothing> initialize(const Flags& flags)
{
  static Once* initialized = new Once();

  if (initialized->once()) {
    return Nothing();
  }

  if (!systemd::exists()) {
    return Error("systemd does not exist on this system");
  }

  systemd_flags = new Flags(flags);

  // Do not initialize any state if we do not have systemd support enabled.
  if (!systemd_flags->enabled) {
    initialized->done();
    return Nothing();
  }

  // If flags->runtime_directory doesn't exist, then we can't proceed.
  if (!os::exists(CHECK_NOTNULL(systemd_flags)->runtime_directory)) {
    return Error("Failed to locate systemd runtime directory: " +
                 CHECK_NOTNULL(systemd_flags)->runtime_directory);
  }

  // On systemd environments we currently migrate executor pids and processes
  // that need to live alongside the executor into a separate executor slice.
  // This allows the life-time of the process to be extended past the life-time
  // of the slave. See MESOS-3352.
  // This function takes responsibility for creating and starting this slice.
  // We inject a `Subprocess::ParentHook` into the `subprocess` function that
  // migrates pids into this slice if the `EXTEND_LIFETIME` option is set on the
  // `subprocess` call.

  // Ensure that the `MESOS_EXECUTORS_SLICE` exists and is running.
  // TODO(jmlvanre): Prevent racing between multiple agents for this creation
  // logic.

  // Check whether the `MESOS_EXECUTORS_SLICE` already exists. Create it if
  // it does not exist.
  // We explicitly don't modify the file if it exists in case operators want
  // to over-ride the settings for the slice that we provide when we create
  // the `Unit` below.
  const Path path(path::join(
      systemd::runtimeDirectory(),
      mesos::MESOS_EXECUTORS_SLICE));

  if (!systemd::slices::exists(path)) {
    // A simple systemd file to allow us to start a new slice.
    string unit = "[Unit]\nDescription=Mesos Executors Slice\n";

    Try<Nothing> create = systemd::slices::create(path, unit);

    if (create.isError()) {
      return Error("Failed to create systemd slice '" +
                   stringify(mesos::MESOS_EXECUTORS_SLICE) +
                   "': " + create.error());
    }
  }

  // Regardless of whether we created the file or it existed already, we
  // `start` the executor slice. It is safe (a no-op) to `start` an already
  // running slice.
  Try<Nothing> start = systemd::slices::start(mesos::MESOS_EXECUTORS_SLICE);

  if (start.isError()) {
    return Error("Failed to start '" +
                 stringify(mesos::MESOS_EXECUTORS_SLICE) +
                 "': " + start.error());
  }

  // Now the `MESOS_EXECUTORS_SLICE` is ready for us to assign any pids. We can
  // verify that our cgroups assignments will work by testing the hierarchy.
  Try<Nothing> cgroupsVerify = cgroups::verify(
      systemd::hierarchy(),
      mesos::MESOS_EXECUTORS_SLICE);

  if (cgroupsVerify.isError()) {
    return Error("Failed to locate systemd cgroups hierarchy: " +
                 cgroupsVerify.error());
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
                   << (realpath.isError() ? realpath.error()
                                          : "does not exist");

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


bool enabled()
{
  return systemd_flags != nullptr && flags().enabled && exists();
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
  Try<string> daemonReload = os::shell("systemctl daemon-reload");
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
    return Error("Failed to write systemd slice `" + path.string() + "`: " +
                 write.error());
  }

  LOG(INFO) << "Created systemd slice: `" << path << "`";

  Try<Nothing> reload = daemonReload();
  if (reload.isError()) {
    return Error("Failed to create systemd slice `" + path.string() + "`: " +
                 reload.error());
  }

  return Nothing();
}


Try<Nothing> start(const string& name)
{
  Try<string> start = os::shell("systemctl start " + name);

  if (start.isError()) {
    return Error(
        "Failed to start systemd slice `" + name + "`: " + start.error());
  }

  LOG(INFO) << "Started systemd slice `" << name << "`";

  return Nothing();
}

} // namespace slices {


namespace socket_activation {

// See `src/libsystemd/sd-daemon/sd-daemon.c` in the systemd source tree
// for the reference implementation. We follow that implementation to
// decide which conditions should result in errors and which should return
// an empty array.
Try<std::vector<int>> listenFds()
{
  vector<int> result;

  Option<std::string> listenPidEnv = os::getenv("LISTEN_PID");
  if (listenPidEnv.isNone()) {
    return result;
  }

  Try<pid_t> listenPid = flags::parse<pid_t>(listenPidEnv.get());
  if (listenPid.isError()) {
    return Error("Could not parse $LISTEN_PID=\"" + listenPidEnv.get() +
                 "\" as integer");
  }

  pid_t pid = ::getpid();
  if (listenPid.get() != pid) {
    LOG(WARNING) << "File descriptors were passed for pid " << listenPid.get()
                 << ", ignoring them because we have pid " << pid;
    return result;
  }

  Option<std::string> listenFdsEnv = os::getenv("LISTEN_FDS");
  if (listenFdsEnv.isNone()) {
    return result;
  }

  Try<int> listenFds = flags::parse<int>(listenFdsEnv.get());
  if (listenFds.isError()) {
    return Error("Could not parse $LISTEN_FDS=\"" + listenFdsEnv.get() +
                 "\" as integer");
  }

  int n = listenFds.get();
  if (n <= 0 || n > INT_MAX - SD_LISTEN_FDS_START) {
    return Error("Too many passed file descriptors");
  }

  for (int fd = SD_LISTEN_FDS_START; fd < SD_LISTEN_FDS_START + n; ++fd) {
    Try<Nothing> cloexec = os::cloexec(fd);
    if (cloexec.isError()) {
      return Error(
          "Could not set CLOEXEC flag for file descriptor " + stringify(fd) +
          cloexec.error());
    }
  }

  result.resize(n);
  std::iota(result.begin(), result.begin() + n, 3);

  return result;
}


Try<std::vector<int>> listenFdsWithNames(const hashset<string>& names)
{
  Try<std::vector<int>> fds = listenFds();
  if (fds.isError()) {
    return fds;
  }

  std::vector<std::string> listenFdnames;

  Option<std::string> listenFdnamesEnv = os::getenv("LISTEN_FDNAMES");
  if (listenFdnamesEnv.isSome()) {
    listenFdnames = strings::split(*listenFdnamesEnv, ":");

    if (listenFdnames.size() != fds->size()) {
      return Error("Size mismatch between file descriptors and names");
    }
  } else {
    // If `LISTEN_FDNAMES` is not set in the environment, libsystemd assigns
    // the special name "unknown" to all passed file descriptors.
    // We do the same.
    listenFdnames.resize(fds->size());
    std::fill_n(listenFdnames.begin(), listenFdnames.size(), "unknown");
  }

  std::vector<int> result;
  for (size_t i = 0; i < listenFdnames.size(); ++i) {
    if (names.contains(listenFdnames[i])) {
      result.push_back(fds->at(i));
    }
  }

  return result;
}


void clearEnvironment()
{
  os::unsetenv("LISTEN_PID");
  os::unsetenv("LISTEN_FDS");
  os::unsetenv("LISTEN_FDNAMES");
}

} // namespace socket_activation {
} // namespace systemd {
