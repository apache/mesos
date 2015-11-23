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

#include <stout/flags.hpp>
#include <stout/nothing.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

namespace systemd {

/**
 * Flags to initialize systemd state.
 */
class Flags : public virtual flags::FlagsBase
{
public:
  Flags();

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
