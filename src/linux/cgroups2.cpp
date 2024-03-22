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

#include "linux/cgroups2.hpp"

#include <iterator>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "linux/fs.hpp"

using std::ostream;
using std::set;
using std::string;
using std::vector;

using mesos::internal::fs::MountTable;

namespace cgroups2 {

// Name of the cgroups v2 filesystem as found in /proc/filesystems.
const string FILE_SYSTEM = "cgroup2";

// Mount point for the cgroups2 file system.
const string MOUNT_POINT = "/sys/fs/cgroup";


template <typename T>
Try<T> read(const string& cgroup, const string& control);

template <typename T>
Try<Nothing> write(
    const string& cgroup,
    const string& control,
    const T& value);


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

} // namespace subtree_control {

} // namespace control {


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


template <>
Try<Nothing> write(
    const string& cgroup,
    const string& control,
    const string& value)
{
  return os::write(path::join(cgroups2::path(cgroup), control), value);
}


template <>
Try<Nothing> write(
    const string& cgroup,
    const string& control,
    const uint64_t& value)
{
  return write(cgroup, control, stringify(value));
}


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


Try<Nothing> create(const string& cgroup, bool recursive)
{
  const string path = cgroups2::path(cgroup);

  Try<Nothing> mkdir = os::mkdir(path, recursive);
  if (mkdir.isError()) {
    return Error("Failed to create directory '" + path + "': " + mkdir.error());
  }

  return Nothing();
}


Try<Nothing> destroy(const string& cgroup)
{
  if (!cgroups2::exists(cgroup)) {
    return Error("Cgroup '" + cgroup + "' does not exist");
  }

  const string path = cgroups2::path(cgroup);
  Try<Nothing> rmdir = os::rmdir(path, false);
  if (rmdir.isError()) {
    return Error("Failed to remove directory '" + path + "': " + rmdir.error());
  }

  return Nothing();
}


Try<Nothing> move_process(const string& cgroup, pid_t pid)
{
  const string path = cgroups2::path(cgroup);

  if (!os::exists(path)) {
    return Error("There does not exist a cgroup at '" + path + "'");
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


Try<set<pid_t>> processes(const string& cgroup)
{
  if (!cgroups2::exists(cgroup)) {
    return Error("Cgroup '" + cgroup + "' does not exist");
  }

  Try<string> contents = cgroups2::read<string>(cgroup, control::PROCESSES);
  if (contents.isError()) {
    return Error(
        "Failed to read cgroup.procs in '" + cgroup + "': " + contents.error());
  }

  string trimmed = strings::trim(*contents);
  if (trimmed.empty()) {
    return set<pid_t>();
  }

  set<pid_t> pids;
  foreach (const string& _pid, strings::split(strings::trim(*contents), "\n")) {
    Try<pid_t> pid = numify<pid_t>(strings::trim(_pid));
    if (pid.isError()) {
      return Error("Failed to parse pid: " + pid.error());
    }

    pids.insert(*pid);
  }

  return pids;
}


Try<Nothing> assign(const string& cgroup, pid_t pid)
{
  if (!cgroups2::exists(cgroup)) {
    return Error("Cgroup '" + cgroup + "' does not exist");
  }

  return cgroups2::write(cgroup, control::PROCESSES, stringify(pid));
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
  Try<string> contents =
    cgroups2::read<string>(cgroup, cgroups2::control::SUBTREE_CONTROLLERS);

  if (contents.isError()) {
    return Error(contents.error());
  }

  using State = control::subtree_control::State;
  State control = State::parse(*contents);
  control.enable(controllers);
  return cgroups2::write(
      cgroup,
      control::SUBTREE_CONTROLLERS,
      stringify(control));
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

} // namespace cpu {

} // namespace cgroups2 {
