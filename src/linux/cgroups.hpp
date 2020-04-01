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

#ifndef __CGROUPS_HPP__
#define __CGROUPS_HPP__

#include <stdint.h>
#include <stdlib.h>

#include <set>
#include <string>
#include <vector>

#include <sys/types.h>

#include <process/future.hpp>
#include <process/timeout.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace cgroups {

// Freezing a cgroup may get stuck (see MESOS-1689 for details). To
// workaround, we may want to thaw the cgroup and retry freezing it.
// This is the suggested retry interval.
const Duration FREEZE_RETRY_INTERVAL = Seconds(10);


// Default number of assign attempts when moving threads to a cgroup.
const unsigned int THREAD_ASSIGN_RETRIES = 100;

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


// Returns error if any of the following is true:
// (a) hierarchy is not mounted,
// (b) cgroup does not exist
// (c) control file does not exist.
Try<Nothing> verify(
    const std::string& hierarchy,
    const std::string& cgroup = "",
    const std::string& control = "");


// Check whether cgroups module is enabled on the current machine.
// @return  True if cgroups module is enabled.
//          False if cgroups module is not available.
bool enabled();


// Return the currently active hierarchies.
// @return  A set of active hierarchy paths (e.g., '/cgroup').
//          Error if unexpected happens.
Try<std::set<std::string>> hierarchies();


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
Try<std::set<std::string>> subsystems();


// Return a set of subsystems that are attached to a given hierarchy. An error
// will be returned if the given hierarchy is not currently mounted with a
// cgroups virtual file system. As a result, this function can be used to check
// whether a hierarchy is indeed a cgroups hierarchy root.
// @param   hierarchy   Path to the hierarchy root.
// @return  A set of attached subsystem names.
//          Error otherwise, (e.g., hierarchy does not exist or is not mounted).
Try<std::set<std::string>> subsystems(const std::string& hierarchy);


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


// Unmount the cgroups virtual file system from the given hierarchy
// root. The caller must make sure to remove all cgroups in the
// hierarchy before unmount. This function assumes the given hierarchy
// is currently mounted with a cgroups virtual file system. It will
// return error if the given hierarchy has any cgroups.
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


// Create a cgroup in a given hierarchy. To create a cgroup, one just
// needs to create a directory in the cgroups virtual file system. The
// given cgroup is a relative path to the given hierarchy. This
// function assumes the given hierarchy is valid and is currently
// mounted with a cgroup virtual file system. The function also
// assumes the given cgroup is valid.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   recursive   Will create nested cgroups
// @return  Some if the operation succeeds.
//          Error if the operation fails.
Try<Nothing> create(
    const std::string& hierarchy,
    const std::string& cgroup,
    bool recursive = false);


// Returns true if the given cgroup under a given hierarchy exists.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  True if the cgroup exists.
//          False if the cgroup does not exist.
bool exists(const std::string& hierarchy, const std::string& cgroup);


// Return all the cgroups under the given cgroup of a given hierarchy.
// By default, it returns all the cgroups under the given hierarchy.
// This function assumes that the given hierarchy and cgroup are
// valid. We use a post-order walk here to ease the removal of
// cgroups.
// @param   hierarchy   Path to the hierarchy root.
// @return  A vector of cgroup names.
Try<std::vector<std::string>> get(
    const std::string& hierarchy,
    const std::string& cgroup = "/");


// Send the specified signal to all process in a cgroup. This function
// assumes that the given hierarchy and cgroup are valid.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   signal      The signal to send to all tasks within the cgroup.
// @return  Some on success.
//          Error if something unexpected happens.
Try<Nothing> kill(
    const std::string& hierarchy,
    const std::string& cgroup,
    int signal);


// Read a control file. Control files are the gateway to monitor and
// control cgroups. This function assumes the cgroups virtual file
// systems are properly mounted on the given hierarchy, and the given
// cgroup has been already created properly. The given control file
// name should also be valid.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @return  The value read from the control file.
Try<std::string> read(
    const std::string& hierarchy,
    const std::string& cgroup,
    const std::string& control);


// Write a control file. This function assumes the cgroups virtual
// file systems are properly mounted on the given hierarchy, and the
// given cgroup has been already created properly. The given control
// file name should also be valid.
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


// Check whether a control file is valid under a given cgroup and a
// given hierarchy. This function will return error if the given
// hierarchy is not properly mounted with appropriate subsystems, or
// the given cgroup does not exist, or the control file does not
// exist.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @return  True if the check succeeds.
//          False if the check fails.
bool exists(
    const std::string& hierarchy,
    const std::string& cgroup,
    const std::string& control);


// Return the set of process IDs in a given cgroup under a given
// hierarchy. It assumes the given hierarchy and cgroup are valid.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  The set of process ids.
Try<std::set<pid_t>> processes(
    const std::string& hierarchy,
    const std::string& cgroup);


// Return the set of thread IDs in a given cgroup under a given
// hierarchy. It assumes the given hierarchy and cgroup are valid.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  The set of thread ids.
Try<std::set<pid_t>> threads(
    const std::string& hierarchy,
    const std::string& cgroup);


// Assign a given process specified by its pid to a given cgroup. All
// threads in the pid's threadgroup will also be moved to the cgroup.
// This function assumes the given hierarchy and cgroup are valid.  It
// will return error if the pid has no process associated with it.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   pid         The pid of the given process.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
Try<Nothing> assign(
    const std::string& hierarchy,
    const std::string& cgroup,
    pid_t pid);


// Isolate a given process specified by its 'pid' to a given cgroup by
// both creating the cgroup (recursively) if it doesn't exist and then
// assigning the process to that cgroup. It assumes the hierarchy is
// valid.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   pid         The pid of the given process.
// @return  Nothing if the operation succeeds.
//          Error if the operation fails.
Try<Nothing> isolate(
    const std::string& hierarchy,
    const std::string& cgroup,
    pid_t pid);


namespace event {

// Listen on an event notifier and return a future which will become
// ready when the certain event happens. This function assumes the
// given hierarchy, cgroup and control file are valid.
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

} // namespace event {


// Destroy a cgroup under a given hierarchy. It will also recursively
// destroy any sub-cgroups. If the freezer subsystem is attached to
// the hierarchy, we attempt to kill all tasks in a given cgroup,
// before removing it. Otherwise, we just attempt to remove the
// cgroup. This function assumes the given hierarchy and cgroup are
// valid. It returns error if we failed to destroy any of the cgroups.
// NOTE: If cgroup is "/" (default), all cgroups under the
// hierarchy are destroyed.
// TODO(vinod): Add support for killing tasks when freezer subsystem
// is not present.
// @param   hierarchy Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  A future which will become ready when the operation is done.
//          Error if something unexpected happens.
process::Future<Nothing> destroy(
    const std::string& hierarchy,
    const std::string& cgroup = "/");


// Destroy a cgroup under a given hierarchy. This is a convenience
// function which wraps the cgroups::destroy() to add a timeout: if
// the cgroup(s) cannot be destroyed after timeout the operation will
// be discarded.
process::Future<Nothing> destroy(
    const std::string& hierarchy,
    const std::string& cgroup,
    const Duration& timeout);


// Cleanup the hierarchy, by first destroying all the underlying
// cgroups, unmounting the hierarchy and deleting the mount point.
// @param   hierarchy Path to the hierarchy root.
// @return  A future which will become ready when the operation is done.
//          Error if something unexpected happens.
process::Future<bool> cleanup(const std::string& hierarchy);


// Returns the stat information from the given file. This function
// assumes the given hierarchy and cgroup are valid.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   file        The stat file to read from. (Ex: "memory.stat").
// @return  The stat information parsed from the file.
//          Error if reading or parsing fails.
// TODO(bmahler): Consider namespacing stat for each subsystem (e.g.
// cgroups::memory::stat and cgroups::cpuacct::stat).
Try<hashmap<std::string, uint64_t>> stat(
    const std::string& hierarchy,
    const std::string& cgroup,
    const std::string& file);


// Blkio subsystem.
namespace blkio {

// Returns the cgroup that the specified pid is a member of within the
// hierarchy that the 'blkio' subsystem is mounted, or None if the subsystem
// is not mounted or the pid is not a member of a cgroup.
Result<std::string> cgroup(pid_t pid);


// Wrapper class for dev_t.
class Device
{
public:
  constexpr Device(dev_t device) : value(device) {}
  unsigned int getMajor() const;
  unsigned int getMinor() const;

  inline bool operator==(const Device& that) const
  {
    return value == that.value;
  }

  inline bool operator!=(const Device& that) const
  {
    return value != that.value;
  }

  inline operator dev_t() const { return value; }

public:
  static Try<Device> parse(const std::string& s);

private:
  dev_t value;
};


enum class Operation {
  TOTAL,
  READ,
  WRITE,
  SYNC,
  ASYNC,
  DISCARD,
};


// Entry for a blkio file. The format of each entry can either be:
// 1. <value>
// 2. <dev> <value>
// 3. <dev> <op> <value>
// 4. <op> <value>
//
// For details:
// https://www.kernel.org/doc/Documentation/cgroup-v1/blkio-controller.txt
struct Value
{
  Option<Device> device;
  Option<Operation> op;
  uint64_t value;

  static Try<Value> parse(const std::string& s);
};


namespace cfq {

Try<std::vector<Value>> time(
    const std::string& hierarchy,
    const std::string& cgroup);


Try<std::vector<Value>> time_recursive(
    const std::string& hierarchy,
    const std::string& cgroup);


Try<std::vector<Value>> sectors(
    const std::string& hierarchy,
    const std::string& cgroup);


Try<std::vector<Value>> sectors_recursive(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the total number of bios/requests merged into requests
// belonging to the given cgroup from blkio.io_merged. This function
// assumes the given hierarchy and cgroup are valid.
Try<std::vector<Value>> io_merged(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the total number of bios/requests merged into requests
// belonging to the given cgroup and all its descendants from
// blkio.io_merged_recursive. This function assumes the given
// hierarchy and cgroup are valid.
Try<std::vector<Value>> io_merged_recursive(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the total number of requests queued up in the given
// cgroup from blkio.io_queued. This function assumes the given
// hierarchy and cgroup are valid.
Try<std::vector<Value>> io_queued(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the total number of requests queued up in the given
// cgroup and all its descendants from blkio.io_queued_recursive.
// This function assumes the given hierarchy and cgroup are valid.
Try<std::vector<Value>> io_queued_recursive(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the number of bytes transferred to/from the disk by the
// given cgroup from blkio.io_service_bytes. This function assumes the
// given hierarchy and cgroup are valid.
Try<std::vector<Value>> io_service_bytes(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the number of bytes transferred to/from the disk by the
// given cgroup and all its descendants from
// blkio.io_service_bytes_recursive. This function assumes the given
// hierarchy and cgroup are valid.
Try<std::vector<Value>> io_service_bytes_recursive(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the total amount of time between request dispatch and
// completion by the IOs done by the given cgroup from
// blkio.io_service_time. This function assumes the given hierarchy
// and cgroup are valid.
Try<std::vector<Value>> io_service_time(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the total amount of time between request dispatch and
// completion by the IOs done by the given cgroup and all its
// descendants from blkio.io_service_time_recursive. This function
// assumes the given hierarchy and cgroup are valid.
Try<std::vector<Value>> io_service_time_recursive(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the number of IOs (bio) issued to the disk by the given
// cgroup from blkio.io_serviced. This function assumes the given
// hierarchy and cgroup are valid.
Try<std::vector<Value>> io_serviced(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the number of IOs (bio) issued to the disk by the given
// cgroup and all its descendants from blkio.io_serviced_recursive.
// This function assumes the given hierarchy and cgroup are valid.
Try<std::vector<Value>> io_serviced_recursive(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the total amount of time the IOs for the given cgroup spent
// waiting in the schedule queues for service from blkio.io_wait_time.
// This function assumes the given hierarchy and cgroup are valid.
Try<std::vector<Value>> io_wait_time(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the total amount of time the IOs for the given cgroup and
// all its descendants spent waiting in the scheduler queues for
// service from blkio.io_wait_time_recursive. This function assumes
// the given hierarchy and cgroup are valid.
Try<std::vector<Value>> io_wait_time_recursive(
    const std::string& hierarchy,
    const std::string& cgroup);

} // namespace cfq {


namespace throttle {

// Returns the numbers of bytes transferred to/from the disk for the
// given cgroup from blkio.throttle.io_service_bytes. This function
// assumes the given hierarchy and cgroup are valid.
Try<std::vector<Value>> io_service_bytes(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the numbers of IOs (bio) issued to the disk for the given
// cgroup from blkio.throttle.io_serviced. This function assumes the
// given hierarchy and cgroup are valid.
Try<std::vector<Value>> io_serviced(
    const std::string& hierarchy,
    const std::string& cgroup);

} // namespace throttle {


inline std::ostream& operator<<(std::ostream& stream, const Device& device)
{
  return stream << device.getMajor() << ':' << device.getMinor();
}


inline std::ostream& operator<<(std::ostream& stream, const Operation op)
{
  switch (op) {
    case Operation::TOTAL:
      return stream << "Total";
    case Operation::READ:
      return stream << "Read";
    case Operation::WRITE:
      return stream << "Write";
    case Operation::SYNC:
      return stream << "Sync";
    case Operation::ASYNC:
      return stream << "Async";
    case Operation::DISCARD:
      return stream << "Discard";
  }

  UNREACHABLE();
}


inline std::ostream& operator<<(std::ostream& stream, const Value& value)
{
  if (value.device.isSome()) {
    stream << value.device.get() << ' ';
  }

  if (value.op.isSome()) {
    stream << value.op.get() << ' ';
  }

  return stream << value.value;
}

} // namespace blkio {


// Cpu controls.
namespace cpu {

// Returns the cgroup that the specified pid is a member of within the
// hierarchy that the 'cpu' subsystem is mounted or None if the
// subsystem is not mounted or the pid is not a member of a cgroup.
Result<std::string> cgroup(pid_t pid);


// Sets the cpu shares using cpu.shares. This function assumes the
// given hierarchy and cgroup are valid.
Try<Nothing> shares(
    const std::string& hierarchy,
    const std::string& cgroup,
    uint64_t shares);


// Returns the cpu shares from cpu.shares. This function assumes the
// given hierarchy and cgroup are valid.
Try<uint64_t> shares(
    const std::string& hierarchy,
    const std::string& cgroup);


// Sets the cfs period using cpu.cfs_period_us. This function assumes
// the given hierarchy and cgroup are valid.
Try<Nothing> cfs_period_us(
    const std::string& hierarchy,
    const std::string& cgroup,
    const Duration& duration);


// Returns the cfs quota from cpu.cfs_quota_us. This function assumes
// the given hierarchy and cgroup are valid.
Try<Duration> cfs_quota_us(
    const std::string& hierarchy,
    const std::string& cgroup);


// Sets the cfs quota using cpu.cfs_quota_us. This function assumes
// the given hierarchy and cgroup are valid.
Try<Nothing> cfs_quota_us(
    const std::string& hierarchy,
    const std::string& cgroup,
    const Duration& duration);

} // namespace cpu {


// Cpuacct subsystem.
namespace cpuacct {

// Returns the cgroup that the specified pid is a member of within the
// hierarchy that the 'cpuacct' subsytem is mounted or None if the
// subsystem is not mounted or the pid is not a member of a cgroup.
//
// @param   pid   process id for which cgroup is queried within the cpuacct
//                subsytem.
// @return  Some cgroup in case there was a valid cgroup found for the pid.
//          Error if there was any error in processing.
Result<std::string> cgroup(pid_t pid);


// Encapsulates the 'stat' information exposed by the cpuacct subsystem.
struct Stats
{
  const Duration user;
  const Duration system;
};


// Returns 'Stats' for a given hierarchy and cgroup. This function
// assumes the given hierarchy and cgroup are valid.
//
// @param   hierarchy   hierarchy for the 'cpuacct' subsystem.
// @param   cgroup      cgroup for a given process.
// @return  Some<Stats> if successful.
//          Error in case of any error during processing.
Try<Stats> stat(
    const std::string& hierarchy,
    const std::string& cgroup);

} // namespace cpuacct {


// Memory controls.
namespace memory {

// Returns the cgroup that the specified pid is a member of within the
// hierarchy that the 'memory' subsytem is mounted or None if the
// subsystem is not mounted or the pid is not a member of a cgroup.
Result<std::string> cgroup(pid_t pid);


// Returns the memory limit from memory.limit_in_bytes. This function
// assumes the given hierarchy and cgroup are valid.
Try<Bytes> limit_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup);


// Sets the memory limit using memory.limit_in_bytes. This function
// assumes the given hierarchy and cgroup are valid.
Try<Nothing> limit_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup,
    const Bytes& limit);


// Returns the memory limit from memory.memsw.limit_in_bytes. Returns
// none if memory.memsw.limit_in_bytes is not supported (e.g., when
// swap is turned off). This function assumes the given hierarchy and
// cgroup are valid.
Result<Bytes> memsw_limit_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup);


// Sets the memory limit using memory.memsw.limit_in_bytes. Returns
// false if memory.memsw.limit_in_bytes is not supported (e.g., when
// swap is turned off). This function assumes the given hierarchy and
// cgroup are valid.
Try<bool> memsw_limit_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup,
    const Bytes& limit);


// Returns the soft memory limit from memory.soft_limit_in_bytes. This
// function assumes the given hierarchy and cgroup are valid.
Try<Bytes> soft_limit_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup);


// Sets the soft memory limit using memory.soft_limit_in_bytes. This
// function assumes the given hierarchy and cgroup are valid.
Try<Nothing> soft_limit_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup,
    const Bytes& limit);


// Returns the memory usage from memory.usage_in_bytes. This function
// assumes the given hierarchy and cgroup are valid.
Try<Bytes> usage_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the memory + swap usage from memory.memsw.usage_in_bytes.
// This function assumes the given hierarchy and cgroup are valid.
Try<Bytes> memsw_usage_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup);


// Returns the max memory usage from memory.max_usage_in_bytes. This
// function assumes the given hierarchy and cgroup are valid.
Try<Bytes> max_usage_in_bytes(
    const std::string& hierarchy,
    const std::string& cgroup);


// Out-of-memory (OOM) controls.
namespace oom {

// Listen for an OOM event for the cgroup. This function assumes the
// given hierarchy and cgroup are valid.
process::Future<Nothing> listen(
    const std::string& hierarchy,
    const std::string& cgroup);

// OOM killer controls.
namespace killer {

// Return whether the kernel OOM killer is enabled for the cgroup.
// This function assumes the given hierarchy and cgroup are valid.
Try<bool> enabled(
    const std::string& hierarchy,
    const std::string& cgroup);

// Enable the kernel OOM killer for the cgroup. The control file will
// only be written to if necessary. This function assumes the given
// hierarchy and cgroup are valid.
Try<Nothing> enable(
    const std::string& hierarchy,
    const std::string& cgroup);

// Disable the kernel OOM killer. The control file will only be
// written to if necessary. This function assumes the given hierarchy
// and cgroup are valid.
Try<Nothing> disable(
    const std::string& hierarchy,
    const std::string& cgroup);

} // namespace killer {

} // namespace oom {


// Memory pressure counters.
namespace pressure {

enum Level
{
  LOW,
  MEDIUM,
  CRITICAL
};


std::ostream& operator<<(std::ostream& stream, Level level);


// Forward declaration.
class CounterProcess;


// Counter is a primitive to listen on events of a given memory
// pressure level for a cgroup and keep track of the number of
// occurrence of that event. Use the public 'create' function to
// create a new counter; see 'value' for how to use.
class Counter
{
public:
  // Create a memory pressure counter for the given cgroup on the
  // specified level. This function assumes the given hierarchy and
  // cgroup are valid.
  static Try<process::Owned<Counter>> create(
      const std::string& hierarchy,
      const std::string& cgroup,
      Level level);

  virtual ~Counter();

  // Returns the current accumulated number of occurrences of the
  // pressure event. Returns a failure if any error occurs while
  // monitoring the pressure events, and any subsequent calls to
  // 'value' will return the same failure. In such case, the user
  // should consider creating a new Counter.
  process::Future<uint64_t> value() const;

private:
  Counter(const std::string& hierarchy,
          const std::string& cgroup,
          Level level);

  process::Owned<CounterProcess> process;
};

} // namespace pressure {

} // namespace memory {


// Device controls.
namespace devices {

struct Entry
{
  static Try<Entry> parse(const std::string& s);

  struct Selector
  {
    enum class Type
    {
      ALL,
      BLOCK,
      CHARACTER,
    };

    Type type;
    Option<unsigned int> major; // Matches all `major` numbers if None.
    Option<unsigned int> minor; // Matches all `minor` numbers if None.
  };

  struct Access
  {
    bool read;
    bool write;
    bool mknod;
  };

  Selector selector;
  Access access;
};

std::ostream& operator<<(
    std::ostream& stream,
    const Entry::Selector::Type& type);

std::ostream& operator<<(
    std::ostream& stream,
    const Entry::Selector& selector);

std::ostream& operator<<(
    std::ostream& stream,
    const Entry::Access& access);

std::ostream& operator<<(
    std::ostream& stream,
    const Entry& entry);


bool operator==(
    const Entry::Selector& left,
    const Entry::Selector& right);

bool operator==(
    const Entry::Access& left,
    const Entry::Access& right);

bool operator==(
    const Entry& left,
    const Entry& right);


// Returns the entries within devices.list. This function assumes the
// given hierarchy and cgroup are valid.
Try<std::vector<Entry>> list(
    const std::string& hierarchy,
    const std::string& cgroup);

// Writes the provided `entry` into devices.allow. This function
// assumes the given hierarchy and cgroup are valid.
Try<Nothing> allow(
    const std::string& hierarchy,
    const std::string& cgroup,
    const Entry& entry);

// Writes the provided `entry` into devices.deny. This function
// assumes the given hierarchy and cgroup are valid.
Try<Nothing> deny(
    const std::string& hierarchy,
    const std::string& cgroup,
    const Entry& entry);

} // namespace devices {


// Freezer controls.
// The freezer can be in one of three states:
// 1. THAWED   : No process in the cgroup is frozen.
// 2. FREEZING : Freezing is in progress but not all processes are frozen.
// 3. FROZEN   : All processes are frozen.
namespace freezer {

// Freeze all processes in the given cgroup. This function will return
// a future which will become ready when all processes have been
// frozen (cgroup is in the FROZEN state).  This function assumes the
// given hierarchy and cgroup are valid.
process::Future<Nothing> freeze(
    const std::string& hierarchy,
    const std::string& cgroup);


// Thaw all processes in the given cgroup. This is a revert operation
// of freezer::freeze. This function will return a future which will
// become ready when all processes have been thawed (cgroup is in the
// THAWED state). This function assumes the given hierarchy and cgroup
// are valid.
process::Future<Nothing> thaw(
    const std::string& hierarchy,
    const std::string& cgroup);

} // namespace freezer {


// Net_cls subsystem.
namespace net_cls {

// Read the uint32_t handle set in `net_cls.classid`. This function
// assumes the given hierarchy and cgroup are valid.
Try<uint32_t> classid(
    const std::string& hierarchy,
    const std::string& cgroup);


// Write the uint32_t handle to the `net_cls.classid`. This function
// assumes the given hierarchy and cgroup are valid.
Try<Nothing> classid(
    const std::string& hierarchy,
    const std::string& cgroup,
    const uint32_t handle);

} // namespace net_cls {


// Named hierarchy.
namespace named {

// Returns the cgroup that the specified pid is a member of within the
// given named hierarchy is mounted or None if the named hierarchy is
// not mounted or the pid is not a member of a cgroup.
Result<std::string> cgroup(const std::string& hierarchyName, pid_t pid);

} // namespace named {


} // namespace cgroups {

namespace std {

template <>
struct hash<cgroups::memory::pressure::Level>
{
  typedef size_t result_type;

  typedef cgroups::memory::pressure::Level argument_type;

  result_type operator()(const argument_type& level) const
  {
    // Use the underlying type of the enum as hash value.
    return static_cast<size_t>(level);
  }
};

} // namespace std {

#endif // __CGROUPS_HPP__
