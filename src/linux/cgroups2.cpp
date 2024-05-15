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

#include <fts.h>

#include "linux/cgroups2.hpp"

#include <iterator>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include <process/after.hpp>
#include <process/loop.hpp>
#include <process/pid.hpp>

#include <stout/adaptor.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/unreachable.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "linux/ebpf.hpp"
#include "linux/fs.hpp"

using std::ostream;
using std::set;
using std::string;
using std::vector;

using process::Break;
using process::Continue;
using process::ControlFlow;
using process::Failure;
using process::Future;
using process::loop;

using mesos::internal::fs::MountTable;

namespace cgroups2 {

// Name of the cgroups v2 filesystem as found in /proc/filesystems.
const string FILE_SYSTEM = "cgroup2";

// Mount point for the cgroups2 file system.
const string MOUNT_POINT = "/sys/fs/cgroup";


template <typename T>
Try<T> read(const string& cgroup, const string& control);


template <>
Try<string> read(const string& cgroup, const string& control)
{
  return os::read(path::join(cgroups2::path(cgroup), control));
}


template <>
Try<uint64_t> read(const string& cgroup, const string& control)
{
  Try<string> content = read<string>(cgroup, control);
  if (content.isError()) {
    return Error(content.error());
  }

  return numify<uint64_t>(strings::trim(*content));
}


Try<Nothing> write(
    const string& cgroup,
    const string& control,
    const string& value)
{
  return os::write(path::join(cgroups2::path(cgroup), control), value);
}


Try<Nothing> write(
    const string& cgroup,
    const string& control,
    const uint64_t& value)
{
  return write(cgroup, control, stringify(value));
}

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

namespace subtree_control {

struct State
{
  State() = default;

  // We don't return errors here because enabling something
  // unknown will fail when writing it back out.
  void enable(const vector<string>& controllers)
  {
    foreach (const string& controller, controllers) {
      enable(controller);
    }
  }

  // We don't return errors here because enabling something
  // unknown will fail when writing it back out.
  void enable(const string& controller)
  {
    _disabled.erase(controller);
    _enabled.insert(controller);
  }

  // We don't return errors here since disabling something
  // unknown will fail when writing it back out.
  void disable(const string& controller)
  {
    _enabled.erase(controller);
    _disabled.insert(controller);
  }

  void disable(const set<string>& controllers)
  {
    foreach (const string& controller, controllers) {
      disable(controller);
    }
  }

  set<string> enabled()  const { return _enabled; }
  set<string> disabled() const { return _disabled; }

  bool enabled(const string& controller) const
  {
    return _enabled.find(controller) != _enabled.end();
  }

  static State parse(const string& contents)
  {
    State control;

    // Trim trailing newline.
    const string trimmed = strings::trim(contents);
    if (trimmed.empty()) {
      return control;
    }

    vector<string> controllers = strings::split(trimmed, " ");
    control._enabled.insert(
      std::make_move_iterator(controllers.begin()),
      std::make_move_iterator(controllers.end()));
    return control;
  }

private:
  set<string> _enabled;
  set<string> _disabled;
};


std::ostream& operator<<(std::ostream& stream, const State& state)
{
  foreach (const string& system, state.enabled()) {
    stream << "+" << system << " ";
  }
  foreach (const string& system, state.disabled()) {
    stream << "-" << system << " ";
  }
  return stream;
}


Try<State> read(const string& cgroup)
{
  Try<string> contents =
    cgroups2::read<string>(cgroup, cgroups2::control::SUBTREE_CONTROLLERS);

  if (contents.isError()) {
    return Error(
        "Failed to read 'cgroup.subtree_control' for cgroup '" + cgroup + "': "
        + contents.error());
  }

  return State::parse(*contents);
}


Try<Nothing> write(const string& cgroup, const State& state)
{
  return cgroups2::write(
      cgroup, control::SUBTREE_CONTROLLERS, stringify(state));
}

} // namespace subtree_control {

} // namespace control {


bool enabled()
{
  Try<bool> supported = mesos::internal::fs::supported(cgroups2::FILE_SYSTEM);
  return supported.isSome() && *supported;
}


Try<Nothing> mount()
{
  if (!cgroups2::enabled()) {
    return Error("cgroups2 is not enabled");
  }

  Try<bool> mounted = cgroups2::mounted();
  if (mounted.isError()) {
    return Error("Failed to check if cgroups2 filesystem is mounted: "
                 + mounted.error());
  }
  if (*mounted) {
    return Error("cgroup2 filesystem is already mounted at"
                 " '" + cgroups2::MOUNT_POINT + "'");
  }

  Try<Nothing> mkdir = os::mkdir(cgroups2::MOUNT_POINT);
  if (mkdir.isError()) {
    return Error("Failed to create cgroups2 directory"
                 " '" + cgroups2::MOUNT_POINT + "': " + mkdir.error());
  }

  return mesos::internal::fs::mount(
      None(),
      cgroups2::MOUNT_POINT,
      cgroups2::FILE_SYSTEM,
      0,
      None());
}


Try<bool> mounted()
{
  Try<MountTable> mountTable = MountTable::read("/proc/mounts");
  if (mountTable.isError()) {
    return Error("Failed to read /proc/mounts: " + mountTable.error());
  }

  foreach (MountTable::Entry entry, mountTable->entries) {
    if (entry.type == cgroups2::FILE_SYSTEM) {
      if (entry.dir == MOUNT_POINT) {
        return true;
      }
      return Error("Found cgroups2 mount at an unexpected location"
                   " '" + entry.dir + "'");
    }
  }

  return false;
}


Try<Nothing> unmount()
{
  Try<bool> mounted = cgroups2::mounted();
  if (mounted.isError()) {
    return Error("Failed to check if the cgroup2 filesystem is mounted: "
                 + mounted.error());
  }

  if (!*mounted) {
    return Error("cgroups2 filesystem is not mounted");
  }

  Try<Nothing> result = mesos::internal::fs::unmount(MOUNT_POINT);
  if (result.isError()) {
    return Error("Failed to unmount the cgroup2 hierarchy"
                 " '" + cgroups2::MOUNT_POINT + "': " + result.error());
  }

  Try<Nothing> rmdir = os::rmdir(cgroups2::MOUNT_POINT);
  if (rmdir.isError()) {
    return Error("Failed to remove directory '" + cgroups2::MOUNT_POINT + "': "
                 + rmdir.error());
  }

  return Nothing();
}


bool exists(const string& cgroup)
{
  return os::exists(cgroups2::path(cgroup));
}


Try<set<string>> get(const string& cgroup)
{
  const string& path = cgroups2::path(cgroup);
  char* paths[] = {const_cast<char*>(path.c_str()), nullptr};

  FTS* tree = fts_open(paths, FTS_NOCHDIR, nullptr);
  if (tree == nullptr) {
    return ErrnoError("Failed to start traversing filesystem");
  }

  FTSENT* node;
  set<string> cgroups;
  while ((node = fts_read(tree)) != nullptr) {
    // Use post-order walk here. fts_level is the depth of the traversal,
    // numbered from -1 to N, where the file/dir was found. The traversal root
    // itself is numbered 0. fts_info includes flags for the current node.
    // FTS_DP indicates a directory being visited in postorder.
    if (node->fts_level > 0 && node->fts_info & FTS_DP) {
      string _cgroup = strings::trim(
          node->fts_path + MOUNT_POINT.length(), "/");
      cgroups.insert(_cgroup);
    }
  }

  if (errno != 0) {
    Error error =
      ErrnoError("Failed to read a node while traversing the filesystem");
    fts_close(tree);
    return error;
  }

  if (fts_close(tree) != 0) {
    return ErrnoError("Failed to stop traversing file system");
  }

  return cgroups;
}


Try<Nothing> create(const string& cgroup, bool recursive)
{
  const string path = cgroups2::path(cgroup);

  Try<Nothing> mkdir = os::mkdir(path, recursive);
  if (mkdir.isError()) {
    return Error("Failed to create directory '" + path + "': " + mkdir.error());
  }

  return Nothing();
}


Try<Nothing> kill(const std::string& cgroup)
{
  if (!cgroups2::exists(cgroup)) {
    return Error("Cgroup does not exist");
  }

  return cgroups2::write(cgroup, cgroups2::control::KILL, "1");
}


Future<Nothing> destroy(const string& cgroup)
{
  if (!cgroups2::exists(cgroup)) {
    return Failure("Cgroup '" + cgroup + "' does not exist");
  }

  // To destroy a subtree of cgroups we first kill all of the processes inside
  // of the cgroup and then remove all of the cgroup directories, removing
  // the most deeply nested directories first.

  Try<Nothing> kill = cgroups2::kill(cgroup);
  if (kill.isError()) {
    return Failure("Failed to kill processes in cgroup: " + kill.error());
  }

  // Wait until all of the processes have been killed.
  int retries = 50;
  Future<Nothing> emptied = loop(
    []() { return process::after(Milliseconds(1)); },
    [=](const Nothing&) mutable -> Future<ControlFlow<Nothing>> {
      Try<set<pid_t>> pids = cgroups2::processes(cgroup, true);
      if (pids.isError()) {
        return Failure("Failed to fetch pids in cgroup: " + pids.error());
      }

      if (pids->empty()) {
        return Break();
      }

      --retries;
      if (retries == 0) {
        return Failure("Processes were still found: " + stringify(*pids));
      }

      return Continue();
    });

  return emptied
    .then([=]() -> Future<Nothing> {
      Try<set<string>> cgroups = cgroups2::get(cgroup);
      if (cgroups.isError()) {
        return Failure("Failed to get nested cgroups: " + cgroups.error());
      }

      cgroups->insert(cgroup);

      // Remove the cgroups in bottom-up order.
      foreach (const string& cgroup, adaptor::reverse(*cgroups)) {
        const string path = cgroups2::path(cgroup);

        // Remove the cgroup's directory. If the directory does not exist,
        // ignore the error to protect against races.
        if (::rmdir(path.c_str()) < 0) {
          ErrnoError error = ErrnoError();
          if (error.code != ENOENT) {
            return Failure(
                "Failed to remove directory '" + path + "': " + error.message);
          }
        }
      }

      return Nothing();
  });
}


Try<Nothing> assign(const string& cgroup, pid_t pid)
{
  if (!cgroups2::exists(cgroup)) {
    return Error("Cgroup '" + cgroup + "' does not exist");
  }

  return cgroups2::write(cgroup, control::PROCESSES, stringify(pid));
}


Try<string> cgroup(pid_t pid)
{
  // A process's cgroup membership is listed in /proc/{pid}/cgroup.
  // The format, e.g if the process belongs to /sys/fs/cgroup/foo/bar, is:
  //
  //   0::/foo/bar
  //   or
  //   0::/foo/bar (deleted)
  //
  // See: https://docs.kernel.org/admin-guide/cgroup-v2.html#processes
  // https://man7.org/linux/man-pages/man7/cgroups.7.html
  const string& cgroupFile = path::join("/proc", stringify(pid), "cgroup");
  if (!os::exists(cgroupFile)) {
    return Error("'" + cgroupFile + "' does not exist");
  }

  Try<string> read = os::read(cgroupFile);
  if (read.isError()) {
    return Error("Failed to read '" + cgroupFile + "': " + read.error());
  }

  string content = strings::trim(*read);
  if (!strings::startsWith(content, "0::/")) {
    return Error("process belongs to a v1 cgroup: " + content);
  }

  content = strings::remove(content, "0::/", strings::Mode::PREFIX);
  content = strings::remove(content, " (deleted)", strings::Mode::SUFFIX);

  return content;
}


Try<set<pid_t>> processes(const string& cgroup, bool recursive)
{
  if (!cgroups2::exists(cgroup)) {
    return Error("Cgroup '" + cgroup + "' does not exist");
  }

  set<string> cgroups = {cgroup};

  if (recursive) {
    Try<set<string>> descendants = cgroups2::get(cgroup);
    if (descendants.isError()) {
      return Error("Failed to list cgroups: " + descendants.error());
    }
    cgroups.insert(descendants->begin(), descendants->end());
  }

  set<pid_t> pids;

  foreach (const string& cgroup, cgroups) {
    Try<string> contents = cgroups2::read<string>(cgroup, control::PROCESSES);

    if (contents.isError() && !exists(cgroup)) {
      continue; // Ignore missing cgroups due to races.
    }

    if (contents.isError()) {
      return Error("Failed to read cgroup.procs in '" + cgroup + "': "
                   + contents.error());
    }

    foreach (const string& line, strings::split(*contents, "\n")) {
      if (line.empty()) continue;

      Try<pid_t> pid = numify<pid_t>(line);
      if (pid.isError()) {
        return Error("Failed to parse '" + line + "' as a pid: " + pid.error());
      }

      pids.insert(*pid);
    }
  }

  return pids;
}


Try<set<pid_t>> threads(const string& cgroup)
{
  Try<string> contents = cgroups2::read<string>(cgroup, control::THREADS);
  if (contents.isError()) {
    return Error("Failed to read 'cgroup.threads' in"
                 " '" + cgroup + "': " + contents.error());
  }

  set<pid_t> tids;
  foreach (const string& line, strings::split(*contents, "\n")) {
    if (line.empty()) continue;

    Try<pid_t> tid = numify<pid_t>(line);
    if (tid.isError()) {
      return Error("Failed to parse '" + line + "' as a tid: " + tid.error());
    }

    tids.insert(*tid);
  }

  return tids;
}


string path(const string& cgroup)
{
  return path::join(cgroups2::MOUNT_POINT, cgroup);
}

namespace controllers {

Try<set<string>> available(const string& cgroup)
{
  Try<string> read =
    cgroups2::read<string>(cgroup, cgroups2::control::CONTROLLERS);

  if (read.isError()) {
    return Error("Failed to read cgroup.controllers in '" + cgroup + "': "
                 + read.error());
  }

  // Trim trailing newline.
  const string contents = strings::trim(*read);
  if (contents.empty()) {
    return set<string>();
  }

  vector<string> controllers = strings::split(contents, " ");
  return set<string>(
      std::make_move_iterator(controllers.begin()),
      std::make_move_iterator(controllers.end()));
}


Try<Nothing> enable(const string& cgroup, const vector<string>& controllers)
{
  using State = control::subtree_control::State;
  Try<State> control = cgroups2::control::subtree_control::read(cgroup);

  if (control.isError()) {
    return Error(control.error());
  }

  control->enable(controllers);
  return cgroups2::control::subtree_control::write(cgroup, *control);
}


Try<Nothing> disable(const string& cgroup, const set<string>& controllers)
{
  using State = control::subtree_control::State;
  Try<State> control = cgroups2::control::subtree_control::read(cgroup);

  if (control.isError()) {
    return Error(control.error());
  }

  control->disable(controllers);
  return cgroups2::control::subtree_control::write(cgroup, *control);
}


Try<set<string>> enabled(const string& cgroup)
{
  Try<string> contents =
    cgroups2::read<string>(cgroup, cgroups2::control::SUBTREE_CONTROLLERS);
  if (contents.isError()) {
    return Error("Failed to read 'cgroup.subtree_control' in '" + cgroup + "'"
                 ": " + contents.error());
  }

  using State = control::subtree_control::State;
  State control = State::parse(*contents);
  return control.enabled();
}

} // namespace controllers {

namespace cpu {

BandwidthLimit::BandwidthLimit(Duration _limit, Duration _period)
  : limit{_limit},
    period{_period} {}


Try<BandwidthLimit> parse_bandwidth(const string& content)
{
  // Format
  // -----------------------------
  // $MAX $PERIOD
  // -----------------------------
  // $MAX        Maximum CPU time, in microseconds, processes in the cgroup can
  //             collectively use during one $PERIOD. If set to "max" then there
  //             is no limit.
  //
  // $PERIOD     Length of one period, in microseconds.
  vector<string> split = strings::split(strings::trim(content), " ");
  if (split.size() != 2) {
    return Error("Expected format '$MAX $PERIOD'"
                 " but received '" + content + "'");
  }

  if (split[0] == "max") {
    return cpu::BandwidthLimit();
  }

  Try<Duration> limit = Duration::parse(split[0] + "us");
  if (limit.isError()) {
    return Error("Failed to parse cpu.max's limit of '" + split[0] + "': "
                 + limit.error());
  }

  Try<Duration> period = Duration::parse(split[1] + "us");
  if (period.isError()) {
    return Error("Failed to parse cpu.max's period of '" + split[1] + "': "
                 + period.error());
  }

  return BandwidthLimit(*limit, *period);
}

namespace control {

const std::string IDLE = "cpu.idle";
const std::string MAX = "cpu.max";
const std::string MAX_BURST = "cpu.max.burst";
const std::string PRESSURE = "cpu.pressure";
const std::string STATS = "cpu.stat";
const std::string UCLAMP_MAX = "cpu.uclamp.max";
const std::string UCLAMP_MIN = "cpu.uclamp.min";
const std::string WEIGHT = "cpu.weight";
const std::string WEIGHT_NICE = "cpu.weight.nice";

namespace stat {

Try<Stats> parse(const string& content)
{
  const vector<string> lines = strings::split(content, "\n");
  cpu::Stats stats;

  foreach (const string& line, lines) {
    if (line.empty()) {
      continue;
    }

    vector<string> tokens = strings::split(line, " ");
    if (tokens.size() != 2) {
      return Error("Invalid line format in 'cpu.stat' expected "
                   "<key> <value> received: '" + line + "'");
    }

    const string& field = tokens[0];
    const string& value = tokens[1];

    Try<uint64_t> number = numify<uint64_t>(value);
    if (number.isError()) {
      return Error("Failed to parse '" + field + "': " + number.error());
    }
    Duration duration = Microseconds(static_cast<int64_t>(*number));

    if      (field == "usage_usec")     { stats.usage = duration; }
    else if (field == "user_usec")      { stats.user_time = duration; }
    else if (field == "system_usec")    { stats.system_time = duration; }
    else if (field == "nr_periods")     { stats.periods = *number; }
    else if (field == "nr_throttled")   { stats.throttled = *number; }
    else if (field == "throttled_usec") { stats.throttle_time = duration; }
    else if (field == "nr_burst")       { stats.bursts = *number; }
    else if (field == "burst_usec")     { stats.bursts_time = duration; }
  }

  return stats;
}

} // namespace stat {

} // namespace control {


Try<Nothing> weight(const string& cgroup, uint64_t weight)
{
  if (cgroup == ROOT_CGROUP) {
    return Error("Operation not supported for the root cgroup");
  }

  return cgroups2::write(cgroup, cpu::control::WEIGHT, weight);
}


Try<uint64_t> weight(const string& cgroup)
{
  if (cgroup == ROOT_CGROUP) {
    return Error("Operation not supported for the root cgroup");
  }

  return cgroups2::read<uint64_t>(cgroup, cpu::control::WEIGHT);
}


Try<cpu::Stats> stats(const string& cgroup)
{
  Try<string> content = cgroups2::read<string>(
      cgroup, cgroups2::cpu::control::STATS);

  if (content.isError()) {
    return Error("Failed to read 'cpu.stat' for the cgroup '" + cgroup + "': "
                 + content.error());
  }

  return cpu::control::stat::parse(*content);
}


Try<Nothing> set_max(const string& cgroup, const cpu::BandwidthLimit& limit)
{
  if (cgroup == ROOT_CGROUP) {
    return Error("Operation not supported for the root cgroup");
  }

  if (limit.limit.isNone()) {
    return cgroups2::write(cgroup, cpu::control::MAX, "max");
  }

  if (limit.period.isNone()) {
    return Error("Invalid bandwidth limit: period can only be None"
                 " for a limitless bandwidth limit");
  }

  if (limit.period->ns() < 0 || limit.limit->ns() < 0
      || limit.period->ns() % 1000 > 0 || limit.limit->ns() % 1000 > 0) {
    return Error("Invalid bandwidth limit: period and limit must be"
                 " positive and microsecond level granularity, received"
                 " period=" + stringify(*limit.period)
                 + " limit=" + stringify(*limit.limit));
  }

  return cgroups2::write(
      cgroup,
      cpu::control::MAX,
      stringify(static_cast<uint64_t>(limit.limit->us()))
        + " "
        + stringify(static_cast<uint64_t>(limit.period->us())));
}


Try<cpu::BandwidthLimit> max(const string& cgroup)
{
  if (cgroup == ROOT_CGROUP) {
    return Error("Operation not supported for the root cgroup");
  }

  Try<string> content = cgroups2::read<string>(cgroup, cpu::control::MAX);
  if (content.isError()) {
    return Error("Failed the read 'cpu.max' for cgroup '" + cgroup + "': "
                 + content.error());
  }

  Try<BandwidthLimit> limit = parse_bandwidth(*content);
  if (limit.isError()) {
    return Error("Failed to parse '" + *content + "' as a bandwidth limit: "
                 + limit.error());
  }

  return *limit;
}

} // namespace cpu {

namespace memory {

namespace internal {

// Parse a byte limit from a string.
//
// Format: "max" OR a u64_t string representing bytes.
Result<Bytes> parse_bytelimit(const string& value)
{
  const string trimmed = strings::trim(value);
  if (trimmed == "max") {
    return None();
  }

  Try<uint64_t> bytes = numify<uint64_t>(trimmed);
  if (bytes.isError()) {
    return Error("Failed to numify '" + trimmed + "': " + bytes.error());
  }

  return Bytes(*bytes);
}

} // namespace internal {


namespace control {

const string CURRENT = "memory.current";
const string EVENTS = "memory.events";
const string LOW = "memory.low";
const string HIGH = "memory.high";
const string MAX = "memory.max";
const string MIN = "memory.min";
const string STAT = "memory.stat";

namespace stat {

Try<Stats> parse(const string& content)
{
  Stats stats;

  bool kernel_found = false;
  foreach (const string& line, strings::split(content, "\n")) {
    if (line.empty()) {
      continue;
    }

    vector<string> tokens = strings::split(line, " ");
    if (tokens.size() != 2) {
      return Error("Invalid line format in 'memory.stat'; expected "
                   "<key> <value> received: '" + line + "'");
    }

    const string& key = tokens[0];
    const string& value = tokens[1];

    Try<uint64_t> n = numify<uint64_t>(value);
    if (n.isError()) {
      return Error("Failed to numify '" + value + "': " + n.error());
    }
    const Bytes bytes(*n);

    if      (key == "anon")         { stats.anon          = bytes; }
    else if (key == "file")         { stats.file          = bytes; }
    else if (key == "kernel")       { stats.kernel        = bytes; }
    else if (key == "kernel_stack") { stats.kernel_stack  = bytes; }
    else if (key == "pagetables")   { stats.pagetables    = bytes; }
    else if (key == "sock")         { stats.sock          = bytes; }
    else if (key == "vmalloc")      { stats.vmalloc       = bytes; }
    else if (key == "file_mapped")  { stats.file_mapped   = bytes; }
    else if (key == "slab")         { stats.slab          = bytes; }
    else if (key == "unevictable")  { stats.unevictable   = bytes; }

    kernel_found |= key == "kernel";
  }

  // See Stats::kernel for an explanation of why this can be missing
  // and why we fill it in using these sub-metrics:
  if (!kernel_found) {
    stats.kernel = stats.kernel_stack
      + stats.pagetables
      + stats.sock
      + stats.vmalloc
      + stats.slab;
  }

  return stats;
}

} // namespace stat {

} // namespace control {

namespace events {

Try<Events> parse(const string& content)
{
  Events events;

  foreach (const string& line, strings::split(content, "\n")) {
    if (line.empty()) {
      continue;
    }

    vector<string> tokens = strings::split(line, " ");
    if (tokens.size() != 2) {
      return Error("Invalid line format in 'memory.events' expected "
                   "<key> <value> received: '" + line + "'");
    }

    const string& field = tokens[0];
    const string& value = tokens[1];

    Try<uint64_t> count = numify<uint64_t>(value);
    if (count.isError()) {
      return Error("Failed to numify '" + value + "': " + count.error());
    }

    if      (field == "low")            { events.low            = *count; }
    else if (field == "high")           { events.high           = *count; }
    else if (field == "max")            { events.max            = *count; }
    else if (field == "oom")            { events.oom            = *count; }
    else if (field == "oom_kill")       { events.oom_kill       = *count; }
    else if (field == "oom_group_kill") { events.oom_group_kill = *count; }
  }

  return events;
}

} // namespace events {

Future<Nothing> oom(const string& cgroup)
{
  // TODO(dleamy): Update this to use inotify, rather than polling.
  return loop(
      []() {
        return process::after(Milliseconds(100));
      },
      [=](const Nothing&) -> Future<ControlFlow<Nothing>> {
        Try<string> content = cgroups2::read<string>(cgroup, control::EVENTS);
        if (content.isError()) {
          return Failure("Failed to read 'memory.events': " + content.error());
        }

        Try<Events> events = events::parse(strings::trim(*content));
        if (events.isError()) {
          return Failure("Failed to parse 'memory.events': " + events.error());
        }

        if (events->oom > 0) {
          return Break(Nothing());
        }
        return Continue();
      });
}


Try<Bytes> usage(const string& cgroup)
{
  Try<uint64_t> contents = cgroups2::read<uint64_t>(
      cgroup, memory::control::CURRENT);
  if (contents.isError()) {
    return Error("Failed to read 'memory.current': " + contents.error());
  }

  return Bytes(*contents);
}


Try<Nothing> set_low(const string& cgroup, const Bytes& bytes)
{
  return cgroups2::write(cgroup, control::LOW, bytes.bytes());
}


Try<Bytes> low(const string& cgroup)
{
  Try<uint64_t> contents = cgroups2::read<uint64_t>(cgroup, control::LOW);
  if (contents.isError()) {
    return Error("Failed to read 'memory.low': " + contents.error());
  }

  return Bytes(*contents);
}


Try<Nothing> set_min(const string& cgroup, const Bytes& bytes)
{
  return cgroups2::write(cgroup, control::MIN, bytes.bytes());
}


Try<Bytes> min(const string& cgroup)
{
  Try<uint64_t> contents = cgroups2::read<uint64_t>(cgroup, control::MIN);
  if (contents.isError()) {
    return Error("Failed to read 'memory.min': " + contents.error());
  }

  return Bytes(*contents);
}


Try<Nothing> set_max(const string& cgroup, const Option<Bytes>& limit)
{
  return cgroups2::write(
      cgroup,
      control::MAX,
      limit.isNone() ?  "max" : stringify(limit->bytes()));
}


Result<Bytes> max(const string& cgroup)
{
  Try<string> contents = cgroups2::read<string>(cgroup, control::MAX);
  if (contents.isError()) {
    return Error("Failed to read 'memory.max': " + contents.error());
  }

  return internal::parse_bytelimit(*contents);
}


Try<Nothing> set_high(const string& cgroup, const Option<Bytes>& limit)
{
  return cgroups2::write(
      cgroup,
      control::HIGH,
      limit.isNone() ?  "max" : stringify(limit->bytes()));
}


Result<Bytes> high(const string& cgroup)
{
  Try<string> contents = cgroups2::read<string>(cgroup, control::HIGH);
  if (contents.isError()) {
    return Error("Failed to read 'memory.high': " + contents.error());
  }

  return internal::parse_bytelimit(*contents);
}


Try<Stats> stats(const string& cgroup)
{
  Try<string> contents = cgroups2::read<string>(cgroup, control::STAT);
  if (contents.isError()) {
    return Error("Failed to read 'memory.stat': " + contents.error());
  }

  return control::stat::parse(*contents);
}

} // namespace memory {

namespace devices {

// Utility class to construct an eBPF program to whitelist or blacklist
// select device accesses.
class DeviceProgram
{
public:
  DeviceProgram() : program{ebpf::Program(BPF_PROG_TYPE_CGROUP_DEVICE)}
  {
    // The BPF_PROG_TYPE_CGROUP_DEVICE program takes in
    // `struct bpf_cgroup_dev_ctx*` as input. We extract the fields into
    // registers r2-5.
    //
    // The device type is encoded in the first 16 bits of `access_type` and
    // the access type is encoded in the last 16 bits of `access_type`.
    program.append({
      // r2: Type ('c', 'b', '?')
      BPF_LDX_MEM(
        BPF_W, BPF_REG_2, BPF_REG_1, offsetof(bpf_cgroup_dev_ctx, access_type)),
      BPF_ALU32_IMM(BPF_AND, BPF_REG_2, 0xFFFF),
      // r3: Access ('r', 'w', 'm')
      BPF_LDX_MEM(BPF_W, BPF_REG_3, BPF_REG_1,
        offsetof(bpf_cgroup_dev_ctx, access_type)),
      BPF_ALU32_IMM(BPF_RSH, BPF_REG_3, 16),
      // r4: Major Version
      BPF_LDX_MEM(BPF_W, BPF_REG_4, BPF_REG_1,
        offsetof(bpf_cgroup_dev_ctx, major)),
      // r5: Minor Version
      BPF_LDX_MEM(BPF_W, BPF_REG_5, BPF_REG_1,
        offsetof(bpf_cgroup_dev_ctx, minor)),
    });
  }

  Try<Nothing> allow(const Entry entry) { return addDevice(entry, true);  }
  Try<Nothing>  deny(const Entry entry) { return addDevice(entry, false); }

  ebpf::Program build()
  {
    if (!hasCatchAll) {
      // Exit instructions.
      // If no entry granted access, then deny the access.
      program.append({
        BPF_MOV64_IMM (BPF_REG_0, DENY_ACCESS),
        BPF_EXIT_INSN(),
      });
    }
    return program;
  }

private:
  Try<Nothing> addDevice(const Entry entry, bool allow)
  {
    if (hasCatchAll) {
      return Nothing();
    }

    // We create a block of bytecode with the format:
    // 1. Major Version Check
    // 2. Minor Version Check
    // 3. Type Check
    // 4. Access Check
    // 5. Allow/Deny Access
    //
    // 6. NEXT BLOCK
    //
    // Either:
    // 1. The device access is matched by (1,2,3,4) and the Allow/Deny access
    //    block (5) is executed.
    // 2. One of (1,2,3,4) does not match the requested access and we skip
    //    to the next block (6).

    const Entry::Selector& selector = entry.selector;
    const Entry::Access& access = entry.access;

    bool check_major = selector.major.isSome();
    bool check_minor = selector.minor.isSome();
    bool check_type = selector.type != Entry::Selector::Type::ALL;
    bool check_access = !access.mknod || !access.read || !access.write;

    // Number of instructions to the [NEXT BLOCK]. This is used if a check
    // fails (meaning this entry does not apply) and we want to skip the
    // subsequent checks.
    short jmp_size = 1 + (check_major ? 1 : 0) + (check_minor ? 1 : 0) +
                     (check_access ? 3 : 0) + (check_type ? 1 : 0);

    // Check major version (r4) against entry.
    if (check_major) {
      program.append({
        BPF_JMP_IMM(BPF_JNE, BPF_REG_4, (int)selector.major.get(), jmp_size),
      });
      --jmp_size;
    }

    // Check minor version (r5) against entry.
    if (check_minor) {
      program.append({
        BPF_JMP_IMM(BPF_JNE, BPF_REG_5, (int)selector.minor.get(), jmp_size),
      });
      --jmp_size;
    }

    // Check type (r2) against entry.
    if (check_type) {
      int bpf_type = [selector]() {
        switch (selector.type) {
          case Entry::Selector::Type::BLOCK:     return BPF_DEVCG_DEV_BLOCK;
          case Entry::Selector::Type::CHARACTER: return BPF_DEVCG_DEV_CHAR;
          case Entry::Selector::Type::ALL:       UNREACHABLE();
        }
      }();

      program.append({
        BPF_JMP_IMM(BPF_JNE, BPF_REG_2, bpf_type, jmp_size),
      });
      --jmp_size;
    }

    // Check access (r3) against entry.
    if (check_access) {
      int bpf_access = 0;
      bpf_access |= access.read ? BPF_DEVCG_ACC_READ : 0;
      bpf_access |= access.write ? BPF_DEVCG_ACC_WRITE : 0;
      bpf_access |= access.mknod ? BPF_DEVCG_ACC_MKNOD : 0;

      program.append({
        BPF_MOV32_REG(BPF_REG_1, BPF_REG_3),
        BPF_ALU32_IMM(BPF_AND, BPF_REG_1, bpf_access),
        BPF_JMP_REG(
          BPF_JNE, BPF_REG_1, BPF_REG_3, static_cast<short>(jmp_size - 2)),
      });
      jmp_size -= 3;
    }

    if (!check_major && !check_minor && !check_type && !check_access) {
      // The exit instructions as well as any additional device entries would
      // generate unreachable blocks.
      hasCatchAll = true;
    }

    // Allow/Deny access block.
    program.append({
      BPF_MOV64_IMM(BPF_REG_0, allow ? ALLOW_ACCESS : DENY_ACCESS),
      BPF_EXIT_INSN(),
    });

    return Nothing();
  }

  ebpf::Program program;

  // Whether the program has a device entry that allows or denies ALL accesses.
  // Such cases need to be specially handled because any instructions added
  // after it will be unreachable, and thus will cause the eBPF verifier to
  // reject the program.
  bool hasCatchAll = false;

  static const int ALLOW_ACCESS = 1;
  static const int DENY_ACCESS = 0;
};


Try<Nothing> configure(
    const string& cgroup,
    const vector<Entry>& allow,
    const vector<Entry>& deny)
{
  DeviceProgram program = DeviceProgram();
  foreach (const Entry entry, allow) {
    program.allow(entry);
  }
  foreach (const Entry entry, deny) {
    program.deny(entry);
  }

  Try<Nothing> attach = ebpf::cgroups2::attach(
      cgroups2::path(cgroup),
      program.build());

  if (attach.isError()) {
    return Error("Failed to attach BPF_PROG_TYPE_CGROUP_DEVICE program: " +
                 attach.error());
  }

  return Nothing();
}

} // namespace devices {

} // namespace cgroups2 {
