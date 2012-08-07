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

#include <set>
#include <string>
#include <vector>

#include <sys/types.h>

#include <process/future.hpp>

#include <stout/option.hpp>
#include <stout/try.hpp>

namespace cgroups {


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


// Check whether cgroups module is enabled on the current machine.
// @return  True if cgroups module is enabled.
//          False if cgroups module is not available.
bool enabled();


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
//          Error if some unexpected happens.
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
Try<std::set<std::string> > subsystems(const std::string& hierarchy);


// Create an empty hierarchy and attach the given subsystems to it. This
// function will return error if the path to the hierarchy root already exists.
// Also, the function will return error if a subsystem in the given subsystem
// list has already been attached to another hierarchy. On success, the cgroups
// virtual file system will be mounted with proper subsystems attached.
// @param   hierarchy   Path to the hierarchy root.
// @param   subsystems  Comma-separated subsystem names.
// @return  True if the operation succeeds.
//          Error if the operation fails.
Try<bool> createHierarchy(const std::string& hierarchy,
                          const std::string& subsystems);


// Remove a hierarchy and the directory associated with it. This function will
// return error if the given hierarchy is not valid. Also, it will return error
// if the given hierarchy has cgroups inside.
// @param   hierarchy   Path to the hierarchy root.
// @return  True if the operation succeeds.
//          Error if the operation fails.
Try<bool> removeHierarchy(const std::string& hierarchy);


// Check whether a given directory is a hierarchy root for cgroups.
// @param   hierarchy   Path to the hierarchy root.
// @return  True if the given directory is a hierarchy root.
//          Error if the check fails.
Try<bool> checkHierarchy(const std::string& hierarchy);


// Check whether a given directory is a hierarchy root for cgroups, and whether
// it has proper subsystems attached.
// @param   hierarchy   Path to the hierarchy root.
// @param   subsystems  Comma-separated subsystem names.
// @return  True if the check succeeds.
//          Error if the check fails.
Try<bool> checkHierarchy(const std::string& hierarchy,
                         const std::string& subsystems);


// Create a cgroup under a given hierarchy. This function will return error if
// the given hierarchy is not valid. The cgroup will NOT be created recursively.
// In other words, if the parent cgroup does not exist, this function will just
// return error.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  True if the operation succeeds.
//          Error if the operation fails.
Try<bool> createCgroup(const std::string& hierarchy,
                       const std::string& cgroup);


// Remove a cgroup under a given hierarchy. This function will return error if
// the given hierarchy or the given cgroup is not valid. The cgroup will NOT be
// removed recursively. In other words, if the cgroup has sub-cgroups inside,
// the function will return error. Also, if any process is attached to the
// given cgroup, the removal operation will also fail.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
Try<bool> removeCgroup(const std::string& hierarchy,
                       const std::string& cgroup);


// Check whether a given cgroup under a given hierarchy is valid. This function
// will verify both the given hierarchy and the given cgroup.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  True if the check succeeds.
//          Error if the check fails.
Try<bool> checkCgroup(const std::string& hierarchy,
                      const std::string& cgroup);


// Read a control file. Control files are used to monitor and control cgroups.
// This function will verify all the parameters. If the given hierarchy is not
// properly mounted with appropriate subsystems, or the given cgroup is not
// valid, or the given control file is not valid, the function will return
// error.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @return  The value read from the control file.
Try<std::string> readControl(const std::string& hierarchy,
                             const std::string& cgroup,
                             const std::string& control);


// Write a control file. Parameter checking is similar to readControl.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @param   value       Value to be written.
// @return  True if the operation succeeds.
//          Error if the operation fails.
Try<bool> writeControl(const std::string& hierarchy,
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
// @return  True if the check succeeds.
//          Error if the check fails.
Try<bool> checkControl(const std::string& hierarchy,
                       const std::string& cgroup,
                       const std::string& control);


// Return all the cgroups under the given cgroup of a given hierarchy. By
// default, it returns all the cgroups under the given hierarchy. This function
// will return error if the given hierarchy is not valid.  We use a post-order
// walk here to ease the removal of cgroups.
// @param   hierarchy   Path to the hierarchy root.
// @return  A vector of cgroup names.
Try<std::vector<std::string> > getCgroups(const std::string& hierarchy,
                                          const std::string& cgroup = "/");


// Return the set of process IDs in a given cgroup under a given hierarchy. It
// will return error if the given hierarchy or the given cgroup is not valid.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  The set of process ids.
Try<std::set<pid_t> > getTasks(const std::string& hierarchy,
                               const std::string& cgroup);


// Assign a given process specified by its pid to a given cgroup. This function
// will return error if the given hierarchy or the given cgroup is not valid.
// Also, it will return error if the pid has no process associated with it.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   pid         The pid of the given process.
// @return  True if the operation succeeds.
//          Error if the operation fails.
Try<bool> assignTask(const std::string& hierarchy,
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
//          Error if some unexpected happens.
process::Future<uint64_t> listenEvent(const std::string& hierarchy,
                                      const std::string& cgroup,
                                      const std::string& control,
                                      const Option<std::string>& args =
                                        Option<std::string>::none());

} // namespace cgroups {

#endif // __CGROUPS_HPP__
