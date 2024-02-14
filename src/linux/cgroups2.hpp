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

#ifndef __CGROUPS_V2_HPP__
#define __CGROUPS_V2_HPP__

#include <string>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

namespace cgroups2 {
namespace control {
// Interface files found in all cgroups.

const std::string CONTROLLERS = "cgroup.controllers";
const std::string EVENTS = "cgroup.events";
const std::string FREEZE = "cgroup.freeze";
const std::string IRQ_PRESSURE = "irq.pressure";
const std::string KILL = "cgroup.kill";
const std::string MAX_DEPTH = "cgroup.max.depth";
const std::string MAX_DESCENDANTS = "cgroup.max.descendants";
const std::string PRESSURE = "cgroup.pressure";
const std::string PROCESSES = "cgroup.procs";
const std::string STATS = "cgroup.stat";
const std::string SUBTREE_CONTROLLERS = "cgroup.subtree_control";
const std::string THREADS = "cgroup.threads";
const std::string TYPE = "cgroup.type";

} // namespace control

// Name of the cgroupv2 filesystem as found in /proc/filesystems.
const std::string FILE_SYSTEM = "cgroup2";
// The root cgroup has the same path as the mount point for the cgroup2 file 
// system meaning it's relative path is the empty string.
const std::string ROOT_CGROUP = "";

// Read from a control (e.g. cpu.weight) inside of cgroup.
Try<std::string> read(const std::string& cgroup, const std::string& control);

// Write to a control (e.g. cpu.weight) inside of a cgroup.
Try<Nothing> write(
  const std::string& cgroup, 
  const std::string& control,
  const std::string& value
);

// Checks if cgroups2 is available on the system.
bool enabled();

// Checks if the client has the required permissions to use cgroups2.
bool has_permissions();

// Mount the cgroupv2 hierarchy at the given mount point
// with the provided options. Errors if the mount point
// already exists.
Try<Nothing> mount(const std::string& mountPoint);

// Check if there's an existing cgroup2 hierarchy and return it if found.
// Otherwise, mount() the cgroup2 hierarchy at the provided location with
// the provided options and return the new hierarchy.
Try<std::string> mount_or_create(const std::string& mountPoint);

// Unmount the cgroup2 hierarchy. Assumes that a hierarchy is mounted,
// and will error if there is none. It's the responsibility of the 
// caller to ensure all child cgroups have been destroyed.
Try<Nothing> unmount();

// Cleanup the cgroup2 hierarchy by first destroying all the underlying 
// cgroups, unmounting the hierarchy, and deleting the mount point. Does nothing
// if the cgroup2 hierarchy has already been destroyed.
Try<Nothing> cleanup();

// Entrypoint into cgroups2. Checks that the host supports cgroups2
// and all of the requested subsystems, then mounts the cgroups2 filesystem
// and enables all of the requested subsystems.
Try<Nothing> prepare(
  const std::string &mountPoint,
  const std::vector<std::string> &subsystems
);

// Creates a cgroup off of the base hierarchy, i.e. <ROOT>/<cgroup>.
// cgroup can be a nested cgroup (e.g. foo/bar/baz). If cgroup is a nested
// cgroup and the parent cgroups do not exist, an error will be returned unless
// recursive=true.
Try<Nothing> create(const std::string& cgroup, bool recursive = false);

namespace subsystems {

// Gets the subsystems that can be controlled by the provided cgroup.
// Returns the subsystems available on the system if no cgroup is provided.
Try<std::set<std::string>> available(const std::string& cgroup = ROOT_CGROUP); 

// Checks if the given subsystems can be controlled by the provided cgroup.
Try<bool> available(
  const std::string& cgroup,
  const std::vector<std::string> subsystems
);

// Enables the given subsystems in the cgroup and disable all other subsystems.
// Errors if a requested subsystem is not available.
Try<Nothing> enable(
  const std::string &cgroup,
  const std::vector<std::string>& subsystems
);

} // namespace subsystems

namespace internal {

// Utility for resolving cgroups2 paths. Equivalent to
// path::normalize(path::join(...)). path::join() can't be used
// because a cgroup can be empty (i.e. "") because the mount point
// in cgroup2 is a cgroup itself, giving an empty relative path.
template <typename... Paths>
inline std::string join(const std::string& path, Paths&&... paths) {
  return path::normalize(path::join(path, std::forward<Paths>(paths)...)).get();
}

} // namespace internal
} // namespace cgroups2

#endif // __CGROUPS_V2_HPP__