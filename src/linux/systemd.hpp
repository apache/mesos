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
 * of the child process into a the `MESOS_EXECUTORS_SLICE` in order to
 * extend its life beyond that of the agent.
 *
 * @return Nothing if successful, otherwise Error.
 */
Try<Nothing> extendLifetime(pid_t child);

} // namespace mesos {


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
