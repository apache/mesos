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

#include <set>
#include <string>
#include <vector>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include "linux/cgroups.hpp"

namespace cgroups2 {

// Root cgroup in the cgroup v2 hierarchy. Since the root cgroup has the same
// path as the root mount point its relative path is the empty string.
const std::string ROOT_CGROUP = "";

// Checks if cgroups2 is available on the system.
bool enabled();


// Mounts the cgroups2 file system at /sys/fs/cgroup. Errors if
// the cgroups v2 file system is already mounted.
Try<Nothing> mount();


// Checks if the cgroup2 file systems is mounted at /sys/fs/cgroup,
// returns an error if the mount is found at an unexpected location.
Try<bool> mounted();


// Unmounts the cgroups2 file system from /sys/fs/cgroup. Errors if
// the cgroup2 file system is not mounted at /sys/fs/cgroup. It's the
// responsibility of the caller to ensure all child cgroups have been destroyed.
Try<Nothing> unmount();


// Check if a cgroup exists.
bool exists(const std::string& cgroup);


// Creates a cgroup off of the base hierarchy, i.e. /sys/fs/cgroup/<cgroup>.
// cgroup can be a nested cgroup (e.g. foo/bar/baz). If cgroup is a nested
// cgroup and the parent cgroups do not exist, an error will be returned unless
// recursive=true.
Try<Nothing> create(const std::string& cgroup, bool recursive = false);


// Destroy a cgroup. If the cgroup does not exist or cannot be destroyed,
// e.g. because it contains processes, an error is returned.
Try<Nothing> destroy(const std::string& cgroup);


// Assign a process to a cgroup, by PID, removing the process from its
// current cgroup. Returns an error if the cgroup does not exist.
Try<Nothing> assign(const std::string& cgroup, pid_t pid);


// Get the cgroup that a process is part of, returns a relative path off of
// /sys/fs/cgroup. E.g. For /sys/fs/cgroup/test, this will return "test".
Try<std::string> cgroup(pid_t pid);


// Get the processes inside of a cgroup.
Try<std::set<pid_t>> processes(const std::string& cgroup);


// Get the absolute of a cgroup. The cgroup provided should not start with '/'.
std::string path(const std::string& cgroup);

namespace controllers {

// Gets the controllers that can be controlled by the provided cgroup.
// Providing cgroups2::ROOT_CGROUP will yield the set of controllers available
// on the host.
Try<std::set<std::string>> available(const std::string& cgroup);


// Enables the given controllers in the cgroup and disables all other
// controllers. Errors if a requested controller is not available.
Try<Nothing> enable(
    const std::string& cgroup,
    const std::vector<std::string>& controllers);


// Get all the controllers that are enabled for a cgroup.
Try<std::set<std::string>> enabled(const std::string& cgroup);

} // namespace controllers {

namespace cpu {

// Set the cpu weight for a cgroup, weight must be in the [1, 10000] range.
// Cannot be used for the root cgroup.
Try<Nothing> weight(const std::string& cgroup, uint64_t weight);


// Retrieve the cpu weight for a cgroup, weight will be in the [1, 10000] range.
// Cannot be used for the root cgroup.
Try<uint64_t> weight(const std::string& cgroup);

} // namespace cpu {

namespace devices {

using cgroups::devices::Entry;

// Configure the device access permissions for the cgroup. These permissions
// are hierarchical. I.e. if a parent cgroup does not allow an access then
// 'this' cgroup will be denied access.
Try<Nothing> configure(
    const std::string& cgroup,
    const std::vector<Entry>& allow,
    const std::vector<Entry>& deny);

} // namespace devices {

} // namespace cgroups2 {

#endif // __CGROUPS_V2_HPP__
