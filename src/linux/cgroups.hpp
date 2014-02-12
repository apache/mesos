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

#ifndef __CGROUPS_HPP__
#define __CGROUPS_HPP__

#include <stdint.h>

#include <set>
#include <string>
#include <vector>

#include <sys/types.h>

#include <process/future.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace cgroups {

// Default number of retry attempts when trying to freeze a cgroup.
const unsigned int FREEZE_RETRIES = 50;
const unsigned int EMPTY_WATCHER_RETRIES = 50;


// We use the following notations throughout the cgroups code. The notations
// here are derived from the kernel documentation. More details can be found in
// <kernel-source>/Documentation/cgroups/cgroups.txt.
//
// Hierarchy -  A hierarchy contains a set of cgroups arranged in a tree such
//              that every task in the system is in exactly one of the cgroups
//              in the hierarchy. One or more subsystems can be attached to a
//              hierarchy.
// Subsystem -  A subsystem (e.g. cpu, memory, cpuset, etc) in the kernel. Each
//              subsystem can be attached to only one hierarchy.
// Cgroup    -  A cgroup is just a set of tasks with a set of controls for one
//              or more subsystems.
// Control   -  A control file in a cgroup (e.g. tasks, cpu.shares).


// TODO(idownes): Rework all functions in this file to better support
// separately mounted subsystems.

// Prepare a hierarchy which has the specified subsystem (and only that
// subsystem) mounted and also has the specified cgroup created. Returns the
// hierarchy. Checks are made to ensure that cgroups are supported and that
// nested cgroups can be created.
Try<std::string> prepare(
    const std::string& baseHierarchy,
    const std::string& subsystem,
    const std::string& cgroup);


// Check whether cgroups module is enabled on the current machine.
// @return  True if cgroups module is enabled.
//          False if cgroups module is not available.
bool enabled();


// Return the currently active hierarchies.
// @return  A set of active hierarchy paths (e.g., '/cgroup').
//          Error if unexpected happens.
Try<std::set<std::string> > hierarchies();


// Get an already mounted hierarchy that has 'subsystems' attached.
// This function will return an error if we are unable to find the
// hierarchies or if we are unable to find if the subsystems are
// mounted at a given hierarchy.
// @param subsystems Comma-separated subsystem names.
// @return Path to the hierarchy root, if a hierarchy with all the
//         given subsystems mounted exists.
//         None, if no such hierarchy exists.
//         Error, if the operation fails.
Result<std::string> hierarchy(const std::string& subsystems);


// Check whether all the given subsystems are enabled on the current machine.
// @param   subsystems  Comma-separated subsystem names.
// @return  True if all the given subsystems are enabled.
//          False if any of the given subsystems is not enabled.
//          Error if something unexpected happens.
Try<bool> enabled(const std::string& subsystems);


// Return true if any of the given subsystems is currently attached to a
// hierarchy.
// @param   subsystems  Comma-separated subsystem names.
// @return  True if any of the given subsystems is being attached.
//          False if non of the given subsystems is being attached.
//          Error if something unexpected happens.
Try<bool> busy(const std::string& subsystems);


// Return the currently enabled subsystems.
// @return  A set of enabled subsystem names if succeeds.
//          Error if unexpected happens.
Try<std::set<std::string> > subsystems();


// Return a set of subsystems that are attached to a given hierarchy. An error
// will be returned if the given hierarchy is not currently mounted with a
// cgroups virtual file system. As a result, this function can be used to check
// whether a hierarchy is indeed a cgroups hierarchy root.
// @param   hierarchy   Path to the hierarchy root.
// @return  A set of attached subsystem names.
//          Error otherwise, (e.g., hierarchy does not exist or is not mounted).
Try<std::set<std::string> > subsystems(const std::string& hierarchy);


// Mount a cgroups hierarchy and attach the given subsystems to
// it. This function will return error if the path given for the
// hierarchy already exists.  Also, the function will return error if
// a subsystem in the given subsystem list has already been attached
// to another hierarchy. On success, the cgroups virtual file system
// will be mounted with the proper subsystems attached. On failure,
// mount will be retried the specified number of times.
// @param   hierarchy   Path to the hierarchy root.
// @param   subsystems  Comma-separated subsystem names.
// @param   retry       Number of times to retry mount.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
Try<Nothing> mount(
    const std::string& hierarchy,
    const std::string& subsystems,
    int retry = 0);


// Unmount a hierarchy and remove the directory associated with
// it. This function will return error if the given hierarchy is not
// valid. Also, it will return error if the given hierarchy has
// any cgroups.
// @param   hierarchy   Path to the hierarchy root.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
Try<Nothing> unmount(const std::string& hierarchy);


// Returns true if the given hierarchy root is mounted as a cgroups
// virtual file system with the specified subsystems attached.
// @param   hierarchy   Path to the hierarchy root.
// @return  True if the given directory is a hierarchy root and all of the
//          specified subsystems are attached.
//          False if the directory is not a hierarchy (or doesn't exist)
//          or some of the specified subsystems are not attached.
//          Error if the operation fails.
Try<bool> mounted(
    const std::string& hierarchy,
    const std::string& subsystems = "");


// Create a cgroup under a given hierarchy. This function will return error if
// the given hierarchy is not valid. The cgroup will NOT be created recursively.
// In other words, if the parent cgroup does not exist, this function will just
// return error.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
Try<Nothing> create(const std::string& hierarchy, const std::string& cgroup);


// Remove a cgroup under a given hierarchy. This function will return error if
// the given hierarchy or the given cgroup is not valid. The cgroup will NOT be
// removed recursively. In other words, if the cgroup has sub-cgroups inside,
// the function will return error. Also, if any process is attached to the
// given cgroup, the removal operation will also fail.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
Try<Nothing> remove(const std::string& hierarchy, const std::string& cgroup);


// Returns true if the given cgroup under a given hierarchy exists.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  True if the cgroup exists.
//          False if the cgroup does not exist.
//          Error if the operation fails (i.e., hierarchy is not mounted).
Try<bool> exists(const std::string& hierarchy, const std::string& cgroup);


// Return all the cgroups under the given cgroup of a given hierarchy. By
// default, it returns all the cgroups under the given hierarchy. This function
// will return error if the given hierarchy is not mounted or the cgroup does
// not exist. We use a post-order walk here to ease the removal of cgroups.
// @param   hierarchy   Path to the hierarchy root.
// @return  A vector of cgroup names.
Try<std::vector<std::string> > get(
    const std::string& hierarchy,
    const std::string& cgroup = "/");


// Send the specified signal to all process in a cgroup.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   signal      The signal to send to all tasks within the cgroup.
// @return  Some on success.
//          Error if something unexpected happens.
Try<Nothing> kill(
    const std::string& hierarchy,
    const std::string& cgroup,
    int signal);


// Read a control file. Control files are used to monitor and control cgroups.
// This function will verify all the parameters. If the given hierarchy is not
// properly mounted with appropriate subsystems, or the given cgroup is not
// valid, or the given control file is not valid, the function will return
// error.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @return  The value read from the control file.
Try<std::string> read(
    const std::string& hierarchy,
    const std::string& cgroup,
    const std::string& control);


// Write a control file. Parameter checking is similar to read.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @param   value       Value to be written.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
Try<Nothing> write(
    const std::string& hierarchy,
    const std::string& cgroup,
    const std::string& control,
    const std::string& value);


// Check whether a control file is valid under a given cgroup and a given
// hierarchy. This function will return error if the given hierarchy is not
// properly mounted with appropriate subsystems, or the given cgroup does not
// exist, or the control file does not exist.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @return  Some if the check succeeds.
//          Error if the check fails.
Try<bool> exists(
    const std::string& hierarchy,
    const std::string& cgroup,
    const std::string& control);


// Return the set of process IDs in a given cgroup under a given hierarchy. It
// will return error if the given hierarchy or the given cgroup is not valid.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  The set of process ids.
Try<std::set<pid_t> > processes(
    const std::string& hierarchy,
    const std::string& cgroup);


// Assign a given process specified by its pid to a given cgroup. This function
// will return error if the given hierarchy or the given cgroup is not valid.
// Also, it will return error if the pid has no process associated with it.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   pid         The pid of the given process.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
Try<Nothing> assign(
    const std::string& hierarchy,
    const std::string& cgroup,
    pid_t pid);


// Listen on an event notifier and return a future which will become ready when
// the certain event happens. This function will return a future failure if some
// expected happens (e.g. the given hierarchy does not have the proper
// subsystems attached).
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @param   args        Control specific arguments.
// @return  A future which contains the value read from the file when ready.
//          Error if something unexpected happens.
process::Future<uint64_t> listen(
    const std::string& hierarchy,
    const std::string& cgroup,
    const std::string& control,
    const Option<std::string>& args = Option<std::string>::none());


// Freeze all the processes in a given cgroup. We try to use the freezer
// subsystem implemented in cgroups. More detail can be found in
// <kernel-source>/Documentation/cgroups/freezer-subsystem.txt. This function
// will return a future which will become ready when all the processes have been
// frozen (FROZEN). The future can be discarded to cancel the operation. The
// freezer state after the cancellation is not defined. So the users need to
// read the control file if they need to know the freezer state after the
// cancellation. This function will return future failure if the freezer
// subsystem is not available or it is not attached to the given hierarchy, or
// the given cgroup is not valid, or the given cgroup has already been frozen.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   interval    The time interval between two state check
//                      requests (default: 0.1 seconds).
// @param   retries     Number of retry attempts before giving up. None
//                      indicates infinite retries. (default: 50 attempts).
// @return  A future which will become true when all processes are frozen, or
//          false when all retries have occurred unsuccessfully.
//          Error if something unexpected happens.
process::Future<bool> freeze(
    const std::string& hierarchy,
    const std::string& cgroup,
    const Duration& interval = Milliseconds(100),
    const unsigned int retries = FREEZE_RETRIES);


// Thaw the given cgroup. This is a revert operation of freezeCgroup. It will
// return error if the given cgroup is already thawed. Same as
// freezeCgroup, this function will return a future which can be discarded to
// allow users to cancel the operation.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   interval    The time interval between two state check
//                      requests (default: 0.1 seconds).
// @return  A future which will become ready when all processes are thawed.
//          Error if something unexpected happens.
process::Future<bool> thaw(
    const std::string& hierarchy,
    const std::string& cgroup,
    const Duration& interval = Milliseconds(100));


// Destroy a cgroup under a given hierarchy. It will also recursively
// destroy any sub-cgroups. If the freezer subsystem is attached to
// the hierarchy, we attempt to kill all tasks in a given cgroup,
// before removing it. Otherwise, we just attempt to remove the
// cgroup. This function will return an error if the given hierarchy
// or the given cgroup does not exist or if we failed to destroy any
// of the cgroups.
// NOTE: If cgroup is "/" (default), all cgroups under the
// hierarchy are destroyed.
// TODO(vinod): Add support for killing tasks when freezer subsystem
// is not present.
// @param   hierarchy Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   interval    The time interval between two state check
//                      requests (default: 0.1 seconds).
// @return  A future which will become ready when the operation is done.
//          Error if something unexpected happens.
process::Future<bool> destroy(
    const std::string& hierarchy,
    const std::string& cgroup = "/",
    const Duration& interval = Milliseconds(100));


// Cleanup the hierarchy, by first destroying all the underlying
// cgroups, unmounting the hierarchy and deleting the mount point.
// @param   hierarchy Path to the hierarchy root.
// @return  A future which will become ready when the operation is done.
//          Error if something unexpected happens.
process::Future<bool> cleanup(const std::string& hierarchy);


// Returns the stat information from the given file.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   file        The stat file to read from. (Ex: "memory.stat").
// @return  The stat information parsed from the file.
//          Error if reading or parsing fails.
// TODO(bmahler): Consider namespacing stat for each subsystem (e.g.
// cgroups::memory::stat and cgroups::cpuacct::stat).
Try<hashmap<std::string, uint64_t> > stat(
    const std::string& hierarchy,
    const std::string& cgroup,
    const std::string& file);


// Memory controls.
namespace memory {

// Returns the memory limit from memory.limit_in_bytes.
Try<Bytes> limit_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup);

// Sets the memory limit using memory.limit_in_bytes.
Try<Nothing> limit_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup,
    const Bytes& limit);

// Returns the soft memory limit from memory.soft_limit_in_bytes.
Try<Bytes> soft_limit_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup);

// Sets the soft memory limit using memory.soft_limit_in_bytes.
Try<Nothing> soft_limit_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup,
    const Bytes& limit);

// Returns the memory usage from memory.usage_in_bytes.
Try<Bytes> usage_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup);

// Returns the max memory usage from memory.max_usage_in_bytes.
Try<Bytes> max_usage_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup);

} // namespace memory {

} // namespace cgroups {

#endif // __CGROUPS_HPP__
