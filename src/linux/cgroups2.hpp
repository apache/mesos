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

#include <process/future.hpp>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
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


// Get all of the cgroups under a given cgroup.
Try<std::set<std::string>> get(const std::string& cgroup = ROOT_CGROUP);


// Creates a cgroup off of the base hierarchy, i.e. /sys/fs/cgroup/<cgroup>.
// cgroup can be a nested cgroup (e.g. foo/bar/baz). If cgroup is a nested
// cgroup and the parent cgroups do not exist, an error will be returned unless
// recursive=true.
Try<Nothing> create(const std::string& cgroup, bool recursive = false);


// Recursively kill all of the processes inside of a cgroup and all child
// cgroups with SIGKILL.
Try<Nothing> kill(const std::string& cgroup);


// Recursively destroy a cgroup and all nested cgroups. Processes inside of
// destroyed cgroups are killed with SIGKILL.
process::Future<Nothing> destroy(const std::string& cgroup);


// Assign a process to a cgroup, by PID, removing the process from its
// current cgroup. Returns an error if the cgroup does not exist.
Try<Nothing> assign(const std::string& cgroup, pid_t pid);


// Get the cgroup that a process is part of, returns a relative path off of
// /sys/fs/cgroup. E.g. For /sys/fs/cgroup/test, this will return "test".
Try<std::string> cgroup(pid_t pid);


// Get the processes inside of a cgroup.
//
// Optionally fetch all of the processes in the cgroup subtree by setting
// recursive=true. This will include all processes in nested cgroups.
Try<std::set<pid_t>> processes(
    const std::string& cgroup, bool recursive = false);


// Get the threads inside of a cgroup.
Try<std::set<pid_t>> threads(const std::string& cgroup);


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
    const std::set<std::string>& controllers);


// Disables controllers in the cgroup. No-op if the controller is not enabled.
Try<Nothing> disable(
    const std::string& cgroup,
    const std::set<std::string>& controllers);


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


// CPU usage statistics for a cgroup.
// Represents a snapshot of the "cpu.stat" control.
struct Stats
{
  // Number of scheduling periods that have elapsed.
  // "nr_periods" key in "cpu.stat"
  // Only available if the "cpu" controller is enabled.
  Option<uint64_t> periods;

  // Number of tasks that have been throttled.
  // "nr_throttled" key in "cpu.stat"
  // Only available if the "cpu" controller is enabled.
  Option<uint64_t> throttled;

  // Total time that tasks have been throttled.
  // "throttled_usec" key in "cpu.stat"
  // Only available if the "cpu" controller is enabled.
  Option<Duration> throttle_time;

  // Number of times tasks have exceeded their burst limit.
  // "nr_burst" key in "cpu.stat"
  // Only available if the "cpu" controller is enabled.
  Option<uint64_t> bursts;

  // Total time tasks have spent while exceeding their burst limit.
  // "burst_usec" key in "cpu.stat"
  // Only available if the "cpu" controller is enabled.
  Option<Duration> bursts_time;

  // Combined time spent in user and kernel space.
  // "usage_usec" key in "cpu.stat"
  Duration usage;

  // Time spent in user space.
  // "user_usec key" in "cpu.stat"
  Duration user_time;

  // Time spent in kernel space.
  // "system_usec" key in "cpu.stat"
  Duration system_time;
};


// Specifies the maximum CPU bandwidth available over a given period.
// Represents a snapshot of the 'cpu.max' control file.
struct BandwidthLimit
{
  // Constructs a limitless bandwidth limit.
  BandwidthLimit() = default;

  // Create a bandwidth limit of `limit` every time `period`.
  BandwidthLimit(Duration limit, Duration period);

  // Maximum CPU time quota (AKA bandwidth) per period.
  Option<Duration> limit;

  // Period where the limit can be used. Can only be None if `limit`
  // is also None, implying that there is no bandwidth limit.
  Option<Duration> period;
};

// Get the CPU usage statistics for a cgroup.
Try<Stats> stats(const std::string& cgroup);


// Set the bandwidth limit for a cgroup.
// Cannot be used for the root cgroup.
Try<Nothing> set_max(const std::string& cgroup, const BandwidthLimit& limit);


// Determine the bandwidth limit for a cgroup.
// Cannot be used for the root cgroup.
Try<BandwidthLimit> max(const std::string& cgroup);

} // namespace cpu {


// [HIERARCHICAL RESTRICTIONS]
//
// If the cgroup2 filesystem is mounted with the 'memory_recursiveprot' option,
// then the memory protections 'memory.min' and 'memory.low' are recursively
// applied to children. For example, if a parent has a 'memory.min' of 1GB then
// the child cgroup cannot reserve more than 1GB of memory. If a child cgroup
// requests more resources than are available to its parent than the child's
// request is capped by their parent's constraints. This aligns with the
// top-down constraint whereby children cannot request more resources than
// their parents. This mount option is enabled by default on most systems,
// including those using systemd.
//
//
// [BYTE ALIGNMENT]
//
// Byte amounts written to the memory controller that are not aligned with the
// system page size, `os::page_size()`, will be rounded down to the nearest
// page size.
//
// Note: This contradicts the official documentation which says that the byte
//       amounts will be rounded up.
//
// See: https://docs.kernel.org/admin-guide/cgroup-v2.html
namespace memory {

// Memory usage statistics.
//
// Snapshot of the 'memory.stat' control file.
//
// Note:
// We only record a subset of the memory statistics; a complete list can be
// found in the kernel documentation.
// https://docs.kernel.org/admin-guide/cgroup-v2.html#memory-interface-files
struct Stats
{
  // Amount of memory used in anonymous mappings such as brk(), sbrk(),
  // and mmap(MAP_ANONYMOUS)
  Bytes anon;

  // Amount of memory used to cache filesystem data, including tmpfs and
  // shared memory.
  Bytes file;

  // Amount of total kernel memory, including (kernel_stack, pagetables,
  // percpu, vmalloc, slab) in addition to other kernel memory use cases.
  //
  // If this field is missing (linux < 5.18), it's calculated as
  // the sum of: kernel_stack, pagetables, sock, vmalloc, and slab. This
  // is an under-accounting since it doesn't include:
  //   - various kvm allocations (e.g. allocated pages to create vcpus)
  //   - io_uring
  //   - tmp_page in pipes during pipe_write()
  //   - bpf ringbuffers
  //   - unix sockets
  // But it's the best measurement we can provide on linux < 5.18.
  // See: https://github.com/torvalds/linux/commit/a8c49af3be5f0b4e105ef6
  Bytes kernel;

  // Amount of memory allocated to kernel stacks.
  Bytes kernel_stack;

  // Amount of memory allocated for page tables.
  Bytes pagetables;

  // Amount of memory used in network transmission buffers.
  Bytes sock;

  // Amount of memory used for vmap backed memory.
  Bytes vmalloc;

  // Amount of cached filesystem data mapped with mmap().
  Bytes file_mapped;

  // Amount of memory used for storing in-kernel data structures.
  Bytes slab;

  // Amount of memory that cannot be reclaimed.
  Bytes unevictable;
};


// Cgroup memory controller events.
//
// Snapshot of the 'memory.events' or 'memory.local.events' control files.
struct Events
{
  // The number of times the cgroup is reclaimed due to high memory pressure
  // even though its usage is under the low boundary. This usually indicates
  // that the low boundary is over-committed.
  uint64_t low;

  // The number of times processes of the cgroup are throttled and routed to
  // perform direct memory reclaim because the high memory boundary was
  // exceeded. For a cgroup whose memory usage is capped by the high limit
  // rather than global memory pressure, this event’s occurrences are expected.
  uint64_t high;

  // The number of times the cgroup’s memory usage was about to go over the
  // max boundary. If direct reclaim fails to bring it down, the cgroup goes
  // to OOM state.
  uint64_t max;

  // The number of times the cgroup’s memory usage was reached the limit and
  // allocation was about to fail. This event is not raised if the OOM killer
  // is not considered as an option, e.g. for failed high-order allocations
  // or if caller asked to not retry attempts.
  uint64_t oom;

  // The number of processes belonging to this cgroup killed by any kind of
  // OOM killer.
  uint64_t oom_kill;

  // The number of times a group OOM has occurred.
  uint64_t oom_group_kill;
};


// Listen for an OOM event for the cgroup or any descendants.
//
// Cannot be used for the root cgroup.
process::Future<Nothing> oom(const std::string& cgroup);


// Current memory usage of a cgroup and its descendants in bytes.
Try<Bytes> usage(const std::string& cgroup);


// Set the best-effort memory protection for a cgroup and its descendants. If
// there is memory contention and this cgroup is within the 'low' threshold,
// then memory will be reclaimed from other cgroups (without memory protection)
// before reclaiming from this cgroup.
//
// Note: See the top-level `cgroups2::memory` comment about byte alignment and
//       hierarchical restrictions.
//
// Cannot be used for the root cgroup.
Try<Nothing> set_low(const std::string& cgroup, const Bytes& bytes);


// Get the best-effort memory protection for a cgroup and its descendants. If
// there is memory contention and this cgroup is within the 'low' threshold,
// then memory will be reclaimed from other cgroups (without memory protection)
// before reclaiming from this cgroup.
//
// Cannot be used for the root cgroup.
Try<Bytes> low(const std::string& cgroup);


// Set the minimum memory that is guaranteed to not be reclaimed under any
// conditions.
//
// Note: See the top-level `cgroups2::memory` comment about byte alignment and
//       hierarchical restrictions.
//
// Cannot be used for the root cgroup.
Try<Nothing> set_min(const std::string& cgroup, const Bytes& bytes);


// Get the minimum memory that is guaranteed to not be reclaimed under any
// conditions.
//
// Cannot be used for the root cgroup.
Try<Bytes> min(const std::string& cgroup);


// Set the maximum memory that can be used by a cgroup and its descendants.
// If the limit is reached and memory cannot be reclaimed, then the OOM
// killer will be invoked on the container.
// If limit is None, then there is no maximum memory limit.
// Cannot be used for the root cgroup.
Try<Nothing> set_max(const std::string& cgroup, const Option<Bytes>& limit);


// Get the maximum memory that can be used by a cgroup and its descendants.
// If the returned limit is None, then there is no maximum memory limit.
// Cannot be used for the root cgroup.
Result<Bytes> max(const std::string& cgroup);


// Set the soft memory limit for a cgroup and its descendants. Exceeding the
// soft limit will cause processes in the cgroup to be throttled and put under
// heavy reclaim pressure.
// If limit is None, then there is no soft memory limit.
// Note: See the top-level `cgroups2::memory` comment about byte alignment.
//
// Cannot be used for the root cgroup.
Try<Nothing> set_high(
    const std::string& cgroup, const Option<Bytes>& limit);


// Get the soft memory limit for a cgroup and its descendants.
// If the returned limit is None, then there is no soft memory limit.
//
// Cannot be used for the root cgroup.
Result<Bytes> high(const std::string& cgroup);


// Get the total memory usage statistics for a cgroup and its descendents.
//
// Cannot be used for the root cgroup.
Try<Stats> stats(const std::string& cgroup);

} // namespace memory {

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
