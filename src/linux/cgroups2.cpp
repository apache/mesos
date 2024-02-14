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
#include <set>
#include <sstream>
 
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "linux/cgroups2.hpp"
#include "linux/fs.hpp"

using std::string;
using std::set;
using std::vector;
using std::stringstream;
using mesos::internal::fs::MountTable;

namespace cgroups2 {

namespace control {

namespace controllers {
// List the available controllers/subsystems in the provided cgroup. 
Try<set<string>> available(const string& cgroup) {
   Try<string> subsystems = cgroups2::read(
    cgroup, 
    cgroups2::control::CONTROLLERS
  );
  if (subsystems.isError()) {
    return Error("Failed to check available subsystems"
                 ": " + subsystems.error());
  }
  vector<string> v = strings::split(subsystems.get(), " ");
  set<string> systems(std::make_move_iterator(v.begin()), 
                      std::make_move_iterator(v.end()));
  return systems;
}

} // namespace controllers

namespace subtree_control {
struct State {
  State() = default;

  // Check if a subsystem is enabled.
  bool enabled(const string& subsystem) {
    return m_enabled.find(subsystem) != m_enabled.end();
  }

  void enable(const vector<string> subsystems) {
    foreach(const string& subsystem, subsystems) {
      enable(subsystem);
    }
  } 

  void enable(const string& subsystem) {
    m_disabled.erase(subsystem);
    m_enabled.insert(subsystem);
  }

  set<string> enabled() const {
    return m_enabled;
  }

  set<string> disabled() const {
    return m_disabled;
  }

  static Try<State> read(const string& cgroup) {
    Try<string> contents = cgroups2::read(
      cgroup, 
      cgroups2::control::SUBTREE_CONTROLLERS
    );
    if (contents.isError()) {
      return Error(contents.error());
    }

    State control;
    vector<string> subsystems = strings::split(contents.get(), " ");
    control.m_enabled.insert(subsystems.begin(), subsystems.end());

    return control;
  }
  
private:
  set<string> m_enabled; 
  set<string> m_disabled;
};  

Try<Nothing> write(const string& cgroup, const State &state) {
  stringstream ss;

  foreach(const string& system, state.enabled()) {
    ss << "+" << system << " ";
  }
  foreach(const string& system, state.disabled()) {
    ss << "-" << system << " ";
  }

  return cgroups2::write(cgroup, control::SUBTREE_CONTROLLERS, ss.str());
}

} // namespace subtree_control

} // namespace control

// Active mount point used by cgroups2. If Some() the expected value is
// expected to be mounted with the cgroup2 file system.
Option<string> rootMountPoint = None();
// Flag to check if Mesos created the mount point or whether an existing
// mount point was used. This impacts the behaviour of unmount(), as a 
// pre-existing mount point will not be destroyed on unmount().
bool createdMountPoint = false;

Try<string> read(const string& cgroup, const string& control) {
  return os::read(cgroups2::internal::join(
    cgroups2::rootMountPoint.get(), 
    cgroup, 
    control
  ));
}

Try<Nothing> write(
  const string& cgroup, 
  const string& control, 
  const string& value
) {
  return os::write(cgroups2::internal::join(
      cgroups2::rootMountPoint.get(), 
      cgroup, 
      control
    ), value
  );
}

bool enabled() {
#ifndef __linux__
  // cgroups(v2) is only available on Linux.
  return false;
#endif
  Try<bool> supported = mesos::internal::fs::supported(cgroups2::FILE_SYSTEM);
  return supported.isSome() && supported.get();
}

bool has_permissions() {
  // Check for root permissions.
  return geteuid() == 0;
}

Try<Nothing> mount(const string& mountPoint) {
  if (!cgroups2::enabled()) {
    return Error("Mounting the cgroups2 hierarchy failed as cgroups2"
                 " is not enabled");
  }
  if (os::exists(mountPoint)) {
    return Error("'" + mountPoint + "' already exists in the file system");
  }

  // Create the directory for the hierarchy.
  Try<Nothing> mkdir = os::mkdir(mountPoint);
  if (mkdir.isError()) {
    return Error("Failed to create cgroups2 directory '" + mountPoint + 
                 "': " + mkdir.error());
  }

  Try<Nothing> result = mesos::internal::fs::mount(
    None(), 
    mountPoint, 
    cgroups2::FILE_SYSTEM, 
    0, 
    None()
  );

  rootMountPoint = mountPoint;
  createdMountPoint = true;
  return Nothing();
}

Try<string> mount_or_create(const std::string& mountPoint) {
  Try<MountTable> mountTable = MountTable::read("/proc/mounts");
  if (mountTable.isError()) {
    return Error(mountTable.error());
  }

  // Check of an existing cgroup2 mount point. 
  foreach (MountTable::Entry entry, mountTable.get().entries) {
    if (entry.type == cgroups2::FILE_SYSTEM) {
      rootMountPoint = entry.dir;
      createdMountPoint = false;
      return entry.dir;
    }
  }

  Try<Nothing> mount = cgroups2::mount(mountPoint);
  if (mount.isError()) {
    return Error(mount.error());
  }

  return mountPoint; 
}

Try<Nothing> unmount() {
  if (rootMountPoint.isNone()) {
    return Error("Failed to unmount the cgroup2 hierarchy because it is "
           "not mounted");
  }
  if (createdMountPoint) {
    Try<Nothing> result = mesos::internal::fs::unmount(
      rootMountPoint.get()     
    );
    if (result.isError()) {
      return Error("Failed to unmount the cgroup2 hierarchy ('" + 
                  rootMountPoint.get() + 
                  "'): " + result.error());
    }

    Try<Nothing> rmdir = os::rmdir(rootMountPoint.get());
    if (rmdir.isError()) {
      return Error(
        "Failed to remove directory '" + rootMountPoint.get() + "': " + 
        rmdir.error());
    }

    rootMountPoint = None();
    createdMountPoint = false;
  }

  return Nothing();
}

Try<Nothing> cleanup() {
  if (rootMountPoint.isNone()) {
    // Hierarchy is not mounted and therefore does not need to be 
    // cleaned up.
    return Nothing();
  }
  
  return cgroups2::unmount();
}

Try<Nothing> prepare(
  const string& mountPoint,
  const vector<string>& subsystems
) {
  if (!cgroups2::enabled()) {
    return Error("No cgroups2 support detected in this kernel");
  }

  if (!cgroups2::has_permissions()) {
    return Error("Using cgroups2 requires root permissions");
  }

  Try<string> mount = cgroups2::mount_or_create(mountPoint);
  if (mount.isError()) {
    return Error(
      "Failed to mount cgroups hierarchy at '" + mountPoint +
      "': " + mount.error());
  }

  Try<bool> available = cgroups2::subsystems::available(ROOT_CGROUP, subsystems);
  if (available.isError()) {
    return Error("Failed to find available subsystems : " + available.error());
  }
  if (!available.get()) {
    return Error("All requested subsystems are not available on the host");
  }

  return cgroups2::subsystems::enable(ROOT_CGROUP, subsystems);
}

Try<Nothing> create(const string& cgroup, bool recursive) {
  Try<Nothing> mkdir = os::mkdir(cgroups2::internal::join(
    rootMountPoint.get(),
    cgroup
  ), recursive);

  if (mkdir.isError()) {
    return Error("Failed to make cgroup \'" + cgroup + "\'");
  }

  return Nothing();
}

namespace subsystems {

Try<set<string>> available(const string& cgroup) {
  return cgroups2::control::controllers::available(cgroup);
}

Try<bool> available(
  const string& cgroup,
  const vector<string> subsystems
) {
  Try<set<string>> available = cgroups2::control::controllers::available(cgroup);
  if (available.isError()) {
    return Error(available.error());
  }

  foreach(string subsystem, subsystems) {
    if (available.get().find(subsystem) == available.get().end()) {
      // Found unavailable subsystem.
      return false;
    }
  }

  return true;
}

Try<Nothing> enable(
  const string& cgroup, 
  const vector<string>& subsystems
) {
  using control::subtree_control::State;

  Try<State> control = State::read(cgroup);
  if (control.isError()) {
    return Error(control.error());
  }

  control.get().enable(subsystems); 
  return control::subtree_control::write(cgroup, control.get());
}

Try<bool> enabled(
  const string& cgroup,
  const vector<string>& subsystems
) {
  using control::subtree_control::State;

  Try<State> control = State::read(cgroup);
  if (control.isError()) {
    return Error(control.error());
  }

  foreach (const string& subsystem, subsystems) {
    if (!control.get().enabled(subsystem)) {
      return false;
    }
  }

  return true;
}

} // namespace subsystems

} // namespace cgroups2
