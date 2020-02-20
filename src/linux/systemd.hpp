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

#ifndef __SYSTEMD_HPP__
#define __SYSTEMD_HPP__

#include <process/subprocess.hpp>

#include <stout/flags.hpp>
#include <stout/hashset.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

namespace systemd {

// TODO(jmlvanre): Consider moving the generic systemd behaviour into
// stout, and leaving the mesos specific behavior here.
namespace mesos {

/**
 * The systemd slice which we use to extend the life of any process
 * which we want to live together with the executor it is associated
 * with, rather than the agent. This allows us to clean up the agent
 * cgroup when the agent terminates without killing any critical
 * components of the executor.
 */
// TODO(jmlvanre): We may want to allow this to be configured.
static const char MESOS_EXECUTORS_SLICE[] = "mesos_executors.slice";


/**
 * A hook that is executed in the parent process. It migrates the pid
 * of the child process into the `MESOS_EXECUTORS_SLICE` in order to
 * extend its life beyond that of the agent.
 *
 * @return Nothing if successful, otherwise Error.
 */
Try<Nothing> extendLifetime(pid_t child);

} // namespace mesos {


namespace socket_activation {

// A re-implementation of the systemd socket activation API.
//
// To implement the socket-passing protocol, systemd uses the
// environment variables `$LISTEN_PID`, `$LISTEN_FDS` and `$LISTEN_FDNAMES`
// according to the scheme documented in [1], [2].
//
// Users of libsystemd can use the following API to interface
// with the socket passing functionality:
//
//     #include <systemd/sd-daemon.h>
//     int sd_listen_fds(int unset_environment);
//     int sd_listen_fds_with_names(int unset_environment, char ***names);
//
// The `sd_listen_fds()` function does the following:
//
//  * The return value is the number of listening sockets passed by
//    systemd. The actual file descriptors of these sockets are
//    numbered 3...n+3.
//  * If the current pid is different from the one specified by the
//    environment variable $LISTEN_PID, 0 is returned
//  * The `CLOEXEC` option will be set on all file descriptors "returned"
//    by this function.
//  * If `unset_environment` is true, the environment variables $LISTEN_PID,
//    $LISTEN_FDS, $LISTEN_FDNAMES will be cleared.
//
// The `sd_listen_fds_with_names()` function does the following:
//
//  * If $LISTEN_FDS is set, will return an array of strings with the
//    names. By default, the name of a socket will be equal to the
//    name of the unit file containing the socket description.
//  * The special string "unknown" is used for sockets where no name
//    could be determined.
//
// For this reimplementation, the interface was slightly changed to better
// suit the needs of the Mesos codebase. However, we still set the `CLOEXEC`
// flag on all file descriptors passed via socket activation when one of
// these functions is called.
//
// [1] https://www.freedesktop.org/software/systemd/man/sd_listen_fds.html#Notes
// [2] http://0pointer.de/blog/projects/socket-activation.html

Try<std::vector<int>> listenFds();

// The names are set by the `FileDescriptorName=` directive in the unit file.
// This requires systemd 227 or newer. Since any number of unit files can
// specify the same name, this can return more than one file descriptor.
Try<std::vector<int>> listenFdsWithNames(const hashset<std::string>& names);

// Clear the `$LISTEN_PID`, `$LISTEN_FDS` and `$LISTEN_FDNAMES` environment
// variables.
//
// *NOTE*: This function is not thread-safe, since it modifies the global
// environment.
void clearEnvironment();

// Defined in `man(3) sd_listen_fds`.
constexpr int SD_LISTEN_FDS_START = 3;

} // namespace socket_activation {


/**
 * Flags to initialize systemd state.
 */
class Flags : public virtual flags::FlagsBase
{
public:
  Flags();

  bool enabled;
  std::string runtime_directory;
  std::string cgroups_hierarchy;
};


const Flags& flags();


/**
 * Initialized state for support of systemd functions in this file.
 *
 * @return Nothing if successful, otherwise Error.
 */
Try<Nothing> initialize(const Flags& flags);


/**
 * Check if we are on a systemd environment by:
 * (1) Testing whether `/sbin/init` links to systemd.
 * (2) Testing whether we have a systemd version.
 * TODO(jmlvanre): This logic can be made more robust, but there does not seem
 * to be a standardized way to test the executing init system in a
 * cross-platform way. The task is made slightly easier because we are only
 * interested in identifying if we are running on systemd, not which specific
 * init system is running.
 *
 * @return Whether running on a systemd environment.
 */
bool exists();


/**
 * Check if systemd exists, and whether we have initialized it.
 */
bool enabled();


/**
 * Returns the path to the runtime directory for systemd units.
 */
Path runtimeDirectory();


/**
 * Return the path to the systemd hierarchy.
 */
Path hierarchy();


/**
 * Runs systemctl daemon-reload.
 *
 * Used after updating configuration files.
 *
 * @return Nothing if successful, otherwise Error.
 */
Try<Nothing> daemonReload();

namespace slices {

/**
 * Returns whether a systemd slice configuration file exists at the given path.
 */
bool exists(const Path& path);


/**
 * Creates a slice configuration with the provided contents at the given path.
 *
 * @param path The path at which to create the slice configurations file.
 * @param data The contents of the configuration file.
 *
 * @return Nothing if successful, otherwise Error.
 */
Try<Nothing> create(const Path& path, const std::string& data);


/**
 * Starts the slice with the given name (via 'systemctl start <name>').
 */
Try<Nothing> start(const std::string& name);

} // namespace slices {

} // namespace systemd {

#endif // __SYSTEMD_HPP__
