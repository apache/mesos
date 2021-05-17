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

#include <errno.h>
#include <fts.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/syscall.h>
#include <sys/types.h>

// This header include must be enclosed in an `extern "C"` block to
// workaround a bug in glibc <= 2.12 (see MESOS-7378).
//
// TODO(gilbert): Remove this when we no longer support glibc <= 2.12.
extern "C" {
#include <sys/sysmacros.h>
}

#include <glog/logging.h>

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <process/after.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/loop.hpp>
#include <process/process.hpp>
#include <process/reap.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/proc.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/realpath.hpp>

#include "linux/cgroups.hpp"
#include "linux/fs.hpp"

using namespace process;

// TODO(benh): Move linux/fs.hpp out of 'mesos- namespace.
using namespace mesos::internal;

using std::dec;
using std::getline;
using std::ifstream;
using std::istringstream;
using std::map;
using std::ofstream;
using std::ostream;
using std::ostringstream;
using std::set;
using std::string;
using std::vector;

namespace cgroups {
namespace internal {

// Snapshot of a subsystem (modeled after a line in /proc/cgroups).
struct SubsystemInfo
{
  SubsystemInfo()
    : hierarchy(0),
      cgroups(0),
      enabled(false) {}

  SubsystemInfo(const string& _name,
                int _hierarchy,
                int _cgroups,
                bool _enabled)
    : name(_name),
      hierarchy(_hierarchy),
      cgroups(_cgroups),
      enabled(_enabled) {}

  string name;      // Name of the subsystem.
  int hierarchy;    // ID of the hierarchy the subsystem is attached to.
  int cgroups;      // Number of cgroups for the subsystem.
  bool enabled;     // Whether the subsystem is enabled or not.
};


// Return information about subsystems on the current machine. We get
// information from /proc/cgroups file. Each line in it describes a
// subsystem.
// @return A map from subsystem names to SubsystemInfo instances if
//         succeeds.  Error if anything unexpected happens.
static Try<map<string, SubsystemInfo>> subsystems()
{
  // TODO(benh): Use os::read to get better error information.
  ifstream file("/proc/cgroups");

  if (!file.is_open()) {
    return Error("Failed to open /proc/cgroups");
  }

  map<string, SubsystemInfo> infos;

  while (!file.eof()) {
    string line;
    getline(file, line);

    if (file.fail()) {
      if (!file.eof()) {
        return Error("Failed to read /proc/cgroups");
      }
    } else {
      if (line.empty()) {
        // Skip empty lines.
        continue;
      } else if (line.find_first_of('#') == 0) {
        // Skip the first line which starts with '#' (contains titles).
        continue;
      } else {
        // Parse line to get subsystem info.
        string name;
        int hierarchy;
        int cgroups;
        bool enabled;

        istringstream ss(line);
        ss >> dec >> name >> hierarchy >> cgroups >> enabled;

        // Check for any read/parse errors.
        if (ss.fail() && !ss.eof()) {
          return Error("Failed to parse /proc/cgroups");
        }

        infos[name] = SubsystemInfo(name, hierarchy, cgroups, enabled);
      }
    }
  }

  return infos;
}


// Mount a cgroups virtual file system (with proper subsystems
// attached) to a given directory (hierarchy root). The cgroups
// virtual file system is the interface exposed by the kernel to
// control cgroups. Each directory created inside the hierarchy root
// is a cgroup. Therefore, cgroups are organized in a tree like
// structure. User can specify what subsystems to be attached to the
// hierarchy root so that these subsystems can be controlled through
// normal file system APIs. A subsystem can only be attached to one
// hierarchy. This function assumes the given hierarchy is an empty
// directory and the given subsystems are enabled in the current
// platform.
// @param   hierarchy   Path to the hierarchy root.
// @param   subsystems  Comma-separated subsystem names.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
static Try<Nothing> mount(const string& hierarchy, const string& subsystems)
{
  if (os::exists(hierarchy)) {
    return Error("'" + hierarchy + "' already exists in the file system");
  }

  // Make sure all subsystems are enabled and not busy.
  foreach (const string& subsystem, strings::tokenize(subsystems, ",")) {
    Try<bool> result = enabled(subsystem);
    if (result.isError()) {
      return Error(result.error());
    } else if (!result.get()) {
      return Error("'" + subsystem + "' is not enabled by the kernel");
    }

    result = busy(subsystem);
    if (result.isError()) {
      return Error(result.error());
    } else if (result.get()) {
      return Error(
          "'" + subsystem + "' is already attached to another hierarchy");
    }
  }

  // Create the directory for the hierarchy.
  Try<Nothing> mkdir = os::mkdir(hierarchy);
  if (mkdir.isError()) {
    return Error(
        "Failed to create directory '" + hierarchy + "': " + mkdir.error());
  }

  // Mount the virtual file system (attach subsystems).
  Try<Nothing> result =
    fs::mount(subsystems, hierarchy, "cgroup", 0, subsystems.c_str());
  if (result.isError()) {
    // Do a best effort rmdir of hierarchy (ignoring success or failure).
    os::rmdir(hierarchy);
    return result;
  }

  return Nothing();
}


// Copies the value of 'cpuset.cpus' and 'cpuset.mems' from a parent
// cgroup to a child cgroup so the child cgroup can actually run tasks
// (otherwise it gets the error 'Device or resource busy').
// @param   hierarchy     Path to hierarchy root.
// @param   parentCgroup  Path to parent cgroup relative to the hierarchy root.
// @param   childCgroup   Path to child cgroup relative to the hierarchy root.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
static Try<Nothing> cloneCpusetCpusMems(
    const string& hierarchy,
    const string& parentCgroup,
    const string& childCgroup)
{
  Try<string> cpus = cgroups::read(hierarchy, parentCgroup, "cpuset.cpus");
  if (cpus.isError()) {
    return Error("Failed to read control 'cpuset.cpus': " + cpus.error());
  }

  Try<string> mems = cgroups::read(hierarchy, parentCgroup, "cpuset.mems");
  if (mems.isError()) {
    return Error("Failed to read control 'cpuset.mems': " + mems.error());
  }

  Try<Nothing> write =
    cgroups::write(hierarchy, childCgroup, "cpuset.cpus", cpus.get());
  if (write.isError()) {
    return Error("Failed to write control 'cpuset.cpus': " + write.error());
  }

  write = cgroups::write(hierarchy, childCgroup, "cpuset.mems", mems.get());
  if (write.isError()) {
    return Error("Failed to write control 'cpuset.mems': " + write.error());
  }

  return Nothing();
}


// Removes a cgroup from a given hierachy.
// @param   hierarchy  Path to hierarchy root.
// @param   cgroup     Path of the cgroup relative to the hierarchy root.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
Future<Nothing> remove(const string& hierarchy, const string& cgroup)
{
  const string path = path::join(hierarchy, cgroup);

  // We retry on EBUSY as a workaround for kernel bug
  // https://lkml.org/lkml/2020/1/15/1349 and others which cause rmdir to fail
  // with EBUSY even though the cgroup appears empty.

  Duration delay = Duration::zero();

  return loop(
      [=]() mutable {
        auto timeout = process::after(delay);
        delay = (delay == Duration::zero()) ? Milliseconds(1) : delay * 2;
        return timeout;
      },
      [=](const Nothing&) mutable -> Future<ControlFlow<Nothing>> {
        if (::rmdir(path.c_str()) == 0) {
          return process::Break();
        } else if (errno == EBUSY) {
          LOG(WARNING) << "Removal of cgroup " << path
                       << " failed with EBUSY, will try again";

          return process::Continue();
        } else {
          // If the `cgroup` still exists in the hierarchy, treat this as
          // an error; otherwise, treat this as a success since the `cgroup`
          // has actually been cleaned up.
          // Save the error string as os::exists may clobber errno.
          const string error = os::strerror(errno);
          if (os::exists(path::join(hierarchy, cgroup))) {
            return Failure(
                "Failed to remove directory '" + path + "': " + error);
          }

          return process::Break();
        }
      }
    );
}


// Removes a list of cgroups from a given hierachy.
// The cgroups are removed in order, so this function can be used to remove a
// cgroup hierarchy in a bottom-up fashion.
// @param   hierarchy  Path to hierarchy root.
// @param   cgroups    Path of the cgroups relative to the hierarchy root.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
Future<Nothing> remove(const string& hierarchy, const vector<string>& cgroups)
{
  Future<Nothing> f = Nothing();

  foreach (const string& cgroup, cgroups) {
    f = f.then([=] {
      return internal::remove(hierarchy, cgroup);
    });
  }

  return f;
}

} // namespace internal {


Try<string> prepare(
    const string& baseHierarchy,
    const string& subsystem,
    const string& cgroup)
{
  // Ensure cgroups are enabled in the kernel.
  if (!cgroups::enabled()) {
    return Error("No cgroups support detected in this kernel");
  }

  // Ensure we have root permissions.
  if (geteuid() != 0) {
    return Error("Using cgroups requires root permissions");
  }

  // Check if the specified subsystem has already been attached to
  // some hierarchy. If not, create and mount the hierarchy according
  // to the given baseHierarchy and subsystem.
  Result<string> hierarchy = cgroups::hierarchy(subsystem);
  if (hierarchy.isError()) {
    return Error(
        "Failed to determine the hierarchy where the subsystem " +
        subsystem + " is attached: " + hierarchy.error());
  }

  if (hierarchy.isNone()) {
    // Attempt to mount the hierarchy ourselves.
    hierarchy = path::join(baseHierarchy, subsystem);

    if (os::exists(hierarchy.get())) {
      // The path specified by the given hierarchy already exists in
      // the file system. We try to remove it if it is an empty
      // directory. This will helps us better deal with slave restarts
      // since we won't need to manually remove the directory.
      Try<Nothing> rmdir = os::rmdir(hierarchy.get(), false);
      if (rmdir.isError()) {
        return Error(
            "Failed to mount cgroups hierarchy at '" + hierarchy.get() +
            "' because we could not remove the existing directory: " +
            rmdir.error());
      }
    }

    // Mount the subsystem.
    Try<Nothing> mount = cgroups::mount(hierarchy.get(), subsystem);
    if (mount.isError()) {
      return Error(
          "Failed to mount cgroups hierarchy at '" + hierarchy.get() +
          "': " + mount.error());
    }
  }

  CHECK_SOME(hierarchy);

  // Create the cgroup if it doesn't exist.
  if (!cgroups::exists(hierarchy.get(), cgroup)) {
    // No cgroup exists, create it.
    Try<Nothing> create = cgroups::create(hierarchy.get(), cgroup, true);
    if (create.isError()) {
      return Error(
          "Failed to create root cgroup " +
          path::join(hierarchy.get(), cgroup) +
          ": " + create.error());
    }
  }

  return hierarchy.get();
}


Try<Nothing> verify(
    const string& hierarchy,
    const string& cgroup,
    const string& control)
{
  Try<bool> mounted = cgroups::mounted(hierarchy);
  if (mounted.isError()) {
    return Error(
        "Failed to determine if the hierarchy at '" + hierarchy +
        "' is mounted: " + mounted.error());
  } else if (!mounted.get()) {
    return Error("'" + hierarchy + "' is not a valid hierarchy");
  }

  if (cgroup != "") {
    if (!os::exists(path::join(hierarchy, cgroup))) {
      return Error("'" + cgroup + "' is not a valid cgroup");
    }
  }

  if (control != "") {
    if (!os::exists(path::join(hierarchy, cgroup, control))) {
      return Error(
          "'" + control + "' is not a valid control (is subsystem attached?)");
    }
  }

  return Nothing();
}


bool enabled()
{
  return os::exists("/proc/cgroups");
}


Try<set<string>> hierarchies()
{
  // Read currently mounted file systems from /proc/mounts.
  Try<fs::MountTable> table = fs::MountTable::read("/proc/mounts");
  if (table.isError()) {
    return Error(table.error());
  }

  set<string> results;
  foreach (const fs::MountTable::Entry& entry, table->entries) {
    if (entry.type == "cgroup") {
      Result<string> realpath = os::realpath(entry.dir);
      if (!realpath.isSome()) {
        return Error(
            "Failed to determine canonical path of " + entry.dir + ": " +
            (realpath.isError()
             ? realpath.error()
             : "No such file or directory"));
      }
      results.insert(realpath.get());
    }
  }

  return results;
}


Result<string> hierarchy(const string& subsystems)
{
  Result<string> hierarchy = None();
  Try<set<string>> hierarchies = cgroups::hierarchies();
  if (hierarchies.isError()) {
    return Error(hierarchies.error());
  }

  foreach (const string& candidate, hierarchies.get()) {
    if (subsystems.empty()) {
      hierarchy = candidate;
      break;
    }

    // Check and see if this candidate meets our subsystem requirements.
    Try<bool> mounted = cgroups::mounted(candidate, subsystems);
    if (mounted.isError()) {
      return Error(mounted.error());
    } else if (mounted.get()) {
      hierarchy = candidate;
      break;
    }
  }

  return hierarchy;
}


Try<bool> enabled(const string& subsystems)
{
  Try<map<string, internal::SubsystemInfo>> infosResult =
    internal::subsystems();
  if (infosResult.isError()) {
    return Error(infosResult.error());
  }

  map<string, internal::SubsystemInfo> infos = infosResult.get();
  bool disabled = false;  // Whether some subsystems are not enabled.

  foreach (const string& subsystem, strings::tokenize(subsystems, ",")) {
    if (infos.find(subsystem) == infos.end()) {
      return Error("'" + subsystem + "' not found");
    }
    if (!infos[subsystem].enabled) {
      // Here, we don't return false immediately because we want to return
      // error if any of the given subsystems is missing.
      disabled = true;
    }
  }

  return !disabled;
}


Try<bool> busy(const string& subsystems)
{
  Try<map<string, internal::SubsystemInfo>> infosResult =
    internal::subsystems();
  if (infosResult.isError()) {
    return Error(infosResult.error());
  }

  map<string, internal::SubsystemInfo> infos = infosResult.get();
  bool busy = false;

  foreach (const string& subsystem, strings::tokenize(subsystems, ",")) {
    if (infos.find(subsystem) == infos.end()) {
      return Error("'" + subsystem + "' not found");
    }
    if (infos[subsystem].hierarchy != 0) {
      // Here, we don't return false immediately because we want to return
      // error if any of the given subsystems is missing.
      busy = true;
    }
  }

  return busy;
}


Try<set<string>> subsystems()
{
  Try<map<string, internal::SubsystemInfo>> infos = internal::subsystems();
  if (infos.isError()) {
    return Error(infos.error());
  }

  set<string> names;
  foreachvalue (const internal::SubsystemInfo& info, infos.get()) {
    if (info.enabled) {
      names.insert(info.name);
    }
  }

  return names;
}


Try<set<string>> subsystems(const string& hierarchy)
{
  // We compare the canonicalized absolute paths.
  Result<string> hierarchyAbsPath = os::realpath(hierarchy);
  if (!hierarchyAbsPath.isSome()) {
    return Error(
        "Failed to determine canonical path of '" + hierarchy + "': " +
        (hierarchyAbsPath.isError()
         ? hierarchyAbsPath.error()
         : "No such file or directory"));
  }

  // Read currently mounted file systems from /proc/mounts.
  Try<fs::MountTable> table = fs::MountTable::read("/proc/mounts");
  if (table.isError()) {
    return Error("Failed to read mount table: " + table.error());
  }

  // Check if hierarchy is a mount point of type cgroup.
  Option<fs::MountTable::Entry> hierarchyEntry;
  foreach (const fs::MountTable::Entry& entry, table->entries) {
    if (entry.type == "cgroup") {
      Result<string> dirAbsPath = os::realpath(entry.dir);
      if (!dirAbsPath.isSome()) {
        return Error(
            "Failed to determine canonical path of '" + entry.dir + "': " +
            (dirAbsPath.isError()
             ? dirAbsPath.error()
             : "No such file or directory"));
      }

      // Seems that a directory can be mounted more than once.
      // Previous mounts are obscured by the later mounts. Therefore,
      // we must see all entries to make sure we find the last one
      // that matches.
      if (dirAbsPath.get() == hierarchyAbsPath.get()) {
        hierarchyEntry = entry;
      }
    }
  }

  if (hierarchyEntry.isNone()) {
    return Error("'" + hierarchy + "' is not a valid hierarchy");
  }

  // Get the intersection of the currently enabled subsystems and
  // mount options. Notice that mount options may contain somethings
  // (e.g. rw) that are not in the set of enabled subsystems.
  Try<set<string>> names = subsystems();
  if (names.isError()) {
    return Error(names.error());
  }

  set<string> result;
  foreach (const string& name, names.get()) {
    if (hierarchyEntry->hasOption(name)) {
      result.insert(name);
    }
  }

  return result;
}


Try<Nothing> mount(const string& hierarchy, const string& subsystems, int retry)
{
  Try<Nothing> mounted = internal::mount(hierarchy, subsystems);

  // TODO(tmarshall) The retry option was added as a fix for a kernel
  // bug in Ubuntu 12.04 that resulted in cgroups not being entirely
  // cleaned up even once they have been completely unmounted from the
  // file system. We should reevaluate this in the future, and
  // hopefully remove it once the bug is no longer an issue.
  if (mounted.isError() && retry > 0) {
    os::sleep(Milliseconds(100));
    return cgroups::mount(hierarchy, subsystems, retry - 1);
  }

  return mounted;
}


Try<Nothing> unmount(const string& hierarchy)
{
  Try<Nothing> unmount = fs::unmount(hierarchy);
  if (unmount.isError()) {
    return unmount;
  }

  Try<Nothing> rmdir = os::rmdir(hierarchy);
  if (rmdir.isError()) {
    return Error(
        "Failed to remove directory '" + hierarchy + "': " + rmdir.error());
  }

  return Nothing();
}


Try<bool> mounted(const string& hierarchy, const string& subsystems)
{
  if (!os::exists(hierarchy)) {
    return false;
  }

  // We compare canonicalized absolute paths.
  Result<string> realpath = os::realpath(hierarchy);
  if (!realpath.isSome()) {
    return Error(
        "Failed to determine canonical path of '" + hierarchy + "': " +
        (realpath.isError()
         ? realpath.error()
         : "No such file or directory"));
  }

  Try<set<string>> hierarchies = cgroups::hierarchies();
  if (hierarchies.isError()) {
    return Error(
        "Failed to get mounted hierarchies: " + hierarchies.error());
  }

  if (hierarchies->count(realpath.get()) == 0) {
    return false;
  }

  // Now make sure all the specified subsytems are attached.
  Try<set<string>> attached = cgroups::subsystems(hierarchy);
  if (attached.isError()) {
    return Error(
        "Failed to get subsystems attached to hierarchy '" +
        hierarchy + "': " + attached.error());
  }

  foreach (const string& subsystem, strings::tokenize(subsystems, ",")) {
    if (attached->count(subsystem) == 0) {
      return false;
    }
  }

  return true;
}


Try<Nothing> create(
    const string& hierarchy,
    const string& cgroup,
    bool recursive)
{
  vector<string> missingCgroups;
  string currentCgroup;
  Path cgroupPath(cgroup);
  for (auto it = cgroupPath.begin(); it != cgroupPath.end(); ++it) {
    currentCgroup = path::join(currentCgroup, *it);
    if (!missingCgroups.empty() ||
        !os::exists(path::join(hierarchy, currentCgroup))) {
      missingCgroups.push_back(currentCgroup);
    }
  }

  string path = path::join(hierarchy, cgroup);

  Try<Nothing> mkdir = os::mkdir(path, recursive);
  if (mkdir.isError()) {
    return Error(
        "Failed to create directory '" + path + "': " + mkdir.error());
  }

  // Now clone 'cpuset.cpus' and 'cpuset.mems' if the 'cpuset'
  // subsystem is attached to the hierarchy.
  Try<set<string>> attached = cgroups::subsystems(hierarchy);
  if (attached.isError()) {
    return Error(
        "Failed to determine if hierarchy '" + hierarchy +
        "' has the 'cpuset' subsystem attached: " + attached.error());
  } else if (attached->count("cpuset") > 0) {
    foreach (const string& cgroup, missingCgroups) {
      string parent = Path(cgroup).dirname();

      Try<Nothing> clone =
        internal::cloneCpusetCpusMems(hierarchy, parent, cgroup);

      if (clone.isError()) {
        return Error(
            "Failed to clone `cpuset.cpus` and `cpuset.mems` from '" +
            parent + "' to '" + cgroup + "': " + clone.error());
      }
    }
  }

  return Nothing();
}


bool exists(const string& hierarchy, const string& cgroup)
{
  return os::exists(path::join(hierarchy, cgroup));
}


Try<vector<string>> get(const string& hierarchy, const string& cgroup)
{
  Result<string> hierarchyAbsPath = os::realpath(hierarchy);
  if (!hierarchyAbsPath.isSome()) {
    return Error(
        "Failed to determine canonical path of '" + hierarchy + "': " +
        (hierarchyAbsPath.isError()
         ? hierarchyAbsPath.error()
         : "No such file or directory"));
  }

  Result<string> destAbsPath = os::realpath(path::join(hierarchy, cgroup));
  if (!destAbsPath.isSome()) {
    return Error(
        "Failed to determine canonical path of '" +
        path::join(hierarchy, cgroup) + "': " +
        (destAbsPath.isError()
         ? destAbsPath.error()
         : "No such file or directory"));
  }

  char* paths[] = {const_cast<char*>(destAbsPath->c_str()), nullptr};

  FTS* tree = fts_open(paths, FTS_NOCHDIR, nullptr);
  if (tree == nullptr) {
    return ErrnoError("Failed to start traversing file system");
  }

  vector<string> cgroups;

  FTSENT* node;
  while ((node = fts_read(tree)) != nullptr) {
    // Use post-order walk here. fts_level is the depth of the traversal,
    // numbered from -1 to N, where the file/dir was found. The traversal root
    // itself is numbered 0. fts_info includes flags for the current node.
    // FTS_DP indicates a directory being visited in postorder.
    if (node->fts_level > 0 && node->fts_info & FTS_DP) {
      string path =
        strings::trim(node->fts_path + hierarchyAbsPath->length(), "/");
      cgroups.push_back(path);
    }
  }

  if (errno != 0) {
    Error error =
      ErrnoError("Failed to read a node while traversing file system");
    fts_close(tree);
    return error;
  }

  if (fts_close(tree) != 0) {
    return ErrnoError("Failed to stop traversing file system");
  }

  return cgroups;
}


Try<Nothing> kill(
    const string& hierarchy,
    const string& cgroup,
    int signal)
{
  Try<set<pid_t>> pids = processes(hierarchy, cgroup);
  if (pids.isError()) {
    return Error("Failed to get processes of cgroup: " + pids.error());
  }

  foreach (pid_t pid, pids.get()) {
    if (::kill(pid, signal) == -1) {
      // If errno is set to ESRCH, it means that either a) this process already
      // terminated, or b) it's in a 'zombie' state and we can't signal it
      // anyway.  In either case, ignore the error.
      if (errno != ESRCH) {
        return ErrnoError(
            "Failed to send " + string(strsignal(signal)) +
            " to process " + stringify(pid));
      }
    }
  }

  return Nothing();
}


Try<string> read(
    const string& hierarchy,
    const string& cgroup,
    const string& control)
{
  string path = path::join(hierarchy, cgroup, control);
  return os::read(path);
}


Try<Nothing> write(
    const string& hierarchy,
    const string& cgroup,
    const string& control,
    const string& value)
{
  string path = path::join(hierarchy, cgroup, control);
  return os::write(path, value);
}


bool exists(
    const string& hierarchy,
    const string& cgroup,
    const string& control)
{
  return os::exists(path::join(hierarchy, cgroup, control));
}


namespace internal {

// Return a set of tasks (schedulable entities) for the cgroup.
// If control == "cgroup.procs" these are processes else
// if control == "tasks" they are all tasks, roughly equivalent to threads.
Try<set<pid_t>> tasks(
    const string& hierarchy,
    const string& cgroup,
    const string& control)
{
  // Note: (from cgroups/cgroups.txt documentation)
  // cgroup.procs: list of thread group IDs in the cgroup. This list is not
  // guaranteed to be sorted or free of duplicate TGIDs, and userspace should
  // sort/uniquify the list if this property is required.
  Try<string> value = cgroups::read(hierarchy, cgroup, control);
  if (value.isError()) {
    return Error("Failed to read cgroups control '" +
                 control + "': " + value.error());
  }

  // Parse the values read from the control file and insert into a set. This
  // ensures they are unique (and also sorted).
  set<pid_t> pids;
  istringstream ss(value.get());
  ss >> dec;
  while (!ss.eof()) {
    pid_t pid;
    ss >> pid;

    if (ss.fail()) {
      if (!ss.eof()) {
        return Error("Failed to parse '" + value.get() + "'");
      }
    } else {
      pids.insert(pid);
    }
  }

  return pids;
}

} // namespace internal {


// NOTE: It is possible for a process pid to be in more than one cgroup if it
// has separate threads (tasks) in different cgroups.
Try<set<pid_t>> processes(const string& hierarchy, const string& cgroup)
{
  return internal::tasks(hierarchy, cgroup, "cgroup.procs");
}


Try<set<pid_t>> threads(const string& hierarchy, const string& cgroup)
{
  return internal::tasks(hierarchy, cgroup, "tasks");
}


Try<Nothing> assign(const string& hierarchy, const string& cgroup, pid_t pid)
{
  return cgroups::write(hierarchy, cgroup, "cgroup.procs", stringify(pid));
}


Try<Nothing> isolate(
    const string& hierarchy,
    const string& cgroup,
    pid_t pid)
{
  // Create cgroup if necessary.
  if (!cgroups::exists(hierarchy, cgroup)) {
    Try<Nothing> create = cgroups::create(hierarchy, cgroup, true);
    if (create.isError()) {
      return Error("Failed to create cgroup: " + create.error());
    }
  }

  Try<Nothing> assign = cgroups::assign(hierarchy, cgroup, pid);
  if (assign.isError()) {
    return Error("Failed to assign process to cgroup: " + assign.error());
  }

  return Nothing();
}


namespace event {

#ifndef EFD_SEMAPHORE
#define EFD_SEMAPHORE (1 << 0)
#endif
#ifndef EFD_CLOEXEC
#define EFD_CLOEXEC 02000000
#endif
#ifndef EFD_NONBLOCK
#define EFD_NONBLOCK 04000
#endif

static int eventfd(unsigned int initval, int flags)
{
#ifdef __NR_eventfd2
  return ::syscall(__NR_eventfd2, initval, flags);
#elif defined(__NR_eventfd)
  int fd = ::syscall(__NR_eventfd, initval);
  if (fd == -1) {
    return -1;
  }

  // Manually set CLOEXEC and NONBLOCK.
  if ((flags & EFD_CLOEXEC) != 0) {
    if (os::cloexec(fd).isError()) {
      os::close(fd);
      return -1;
    }
  }

  if ((flags & EFD_NONBLOCK) != 0) {
    if (os::nonblock(fd).isError()) {
      os::close(fd);
      return -1;
    }
  }

  // Return the file descriptor.
  return fd;
#else
#error "The eventfd syscall is not available."
#endif
}


// In cgroups, there is mechanism which allows to get notifications about
// changing status of a cgroup. It is based on Linux eventfd. See more
// information in the kernel documentation ("Notification API"). This function
// will create an eventfd and write appropriate control file to correlate the
// eventfd with a type of event so that users can start polling on the eventfd
// to get notified. It returns the eventfd (file descriptor) if the notifier has
// been successfully registered. This function assumes all the parameters are
// valid. The eventfd is set to be non-blocking.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @param   args        Control specific arguments.
// @return  The eventfd if the operation succeeds.
//          Error if the operation fails.
static Try<int> registerNotifier(
    const string& hierarchy,
    const string& cgroup,
    const string& control,
    const Option<string>& args = None())
{
  int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (efd < 0) {
    return ErrnoError("Failed to create an eventfd");
  }

  // Open the control file.
  string path = path::join(hierarchy, cgroup, control);
  Try<int> cfd = os::open(path, O_RDWR | O_CLOEXEC);
  if (cfd.isError()) {
    os::close(efd);
    return Error("Failed to open '" + path + "': " + cfd.error());
  }

  // Write the event control file (cgroup.event_control).
  ostringstream out;
  out << dec << efd << " " << cfd.get();
  if (args.isSome()) {
    out << " " << args.get();
  }

  Try<Nothing> write = cgroups::write(
      hierarchy,
      cgroup,
      "cgroup.event_control",
      out.str());

  if (write.isError()) {
    os::close(efd);
    os::close(cfd.get());
    return Error(
        "Failed to write control 'cgroup.event_control': " + write.error());
  }

  os::close(cfd.get());

  return efd;
}


// Unregister a notifier.
// @param   fd      The eventfd returned by registerNotifier.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
static Try<Nothing> unregisterNotifier(int fd)
{
  return os::close(fd);
}


// The process listening on an event notifier. This class is internal
// to the cgroup code and assumes parameters are valid. See the
// comments of the public interface 'listen' for its usage.
class Listener : public Process<Listener>
{
public:
  Listener(const string& _hierarchy,
           const string& _cgroup,
           const string& _control,
           const Option<string>& _args)
    : ProcessBase(ID::generate("cgroups-listener")),
      hierarchy(_hierarchy),
      cgroup(_cgroup),
      control(_control),
      args(_args),
      data(0) {}

  ~Listener() override {}

  // Waits for the next event to occur, at which point the future
  // becomes ready. Returns a failure if error occurs. If any previous
  // call to 'listen' returns a failure, all subsequent calls to
  // 'listen' will return failures as well (in that case, the user
  // should consider terminate this process and create a new one if
  // they still want to monitor the events).
  // TODO(chzhcn): If the user discards the returned future, currently
  // we do not do anything. Consider a better discard semantics here.
  Future<uint64_t> listen()
  {
    if (error.isSome()) {
      return Failure(error.get());
    }

    if (promise.isNone()) {
      promise = Owned<Promise<uint64_t>>(new Promise<uint64_t>());

      // Perform nonblocking read on the event file. The nonblocking
      // read will start polling on the event file until it becomes
      // readable. If we can successfully read 8 bytes (sizeof
      // uint64_t) from the event file, it indicates that an event has
      // occurred.
      reading = io::read(eventfd.get(), &data, sizeof(data));
      reading->onAny(defer(self(), &Listener::_listen, lambda::_1));
    }

    return promise.get()->future();
  }

protected:
  void initialize() override
  {
    // Register an eventfd "notifier" for the given control.
    Try<int> fd = registerNotifier(hierarchy, cgroup, control, args);
    if (fd.isError()) {
      error = Error("Failed to register notification eventfd: " + fd.error());
    } else {
      // Remember the opened event file descriptor.
      eventfd = fd.get();
    }
  }

  void finalize() override
  {
    // Discard the nonblocking read.
    if (reading.isSome()) {
      reading->discard();
    }

    // Unregister the eventfd if needed. If there's a pending read,
    // we must wait for it to finish.
    if (eventfd.isSome()) {
      int fd = eventfd.get();

      reading.getOrElse(Future<size_t>(0))
        .onAny([fd]() {
          Try<Nothing> unregister = unregisterNotifier(fd);
          if (unregister.isError()) {
            LOG(ERROR) << "Failed to unregister eventfd '" << fd << "'"
                       << ": " << unregister.error();
          }
      });
    }

    // TODO(chzhcn): Fail our promise only after 'reading' has
    // completed (ready, failed or discarded).
    if (promise.isSome()) {
      if (promise.get()->future().hasDiscard()) {
        promise.get()->discard();
      } else {
        promise.get()->fail("Event listener is terminating");
      }
    }
  }

private:
  // This function is called when the nonblocking read on the eventfd has
  // result, either because the event has happened, or an error has occurred.
  void _listen(Future<size_t> read)
  {
    CHECK_SOME(promise);
    CHECK_SOME(reading);

    // Reset to none since we're no longer reading.
    reading = None();

    if (read.isReady() && read.get() == sizeof(data)) {
      promise.get()->set(data);

      // After fulfilling the promise, reset to get ready for the next one.
      promise = None();
      return;
    }

    if (read.isDiscarded()) {
      error = Error("Reading eventfd stopped unexpectedly");
    } else if (read.isFailed()) {
      error = Error("Failed to read eventfd: " + read.failure());
    } else {
      error = Error("Read less than expected. Expect " +
                    stringify(sizeof(data)) + " bytes; actual " +
                    stringify(read.get()) + " bytes");
    }

    // Inform failure and not listen again.
    promise.get()->fail(error->message);
  }

  const string hierarchy;
  const string cgroup;
  const string control;
  const Option<string> args;

  Option<Owned<Promise<uint64_t>>> promise;
  Option<Future<size_t>> reading;
  Option<Error> error;
  Option<int> eventfd;
  uint64_t data;                // The data read from the eventfd last time.
};


Future<uint64_t> listen(
    const string& hierarchy,
    const string& cgroup,
    const string& control,
    const Option<string>& args)
{
  Listener* listener = new Listener(hierarchy, cgroup, control, args);

  spawn(listener, true);

  Future<uint64_t> future = dispatch(listener, &Listener::listen);

  // If the user doesn't care any more, or listening has had a result,
  // terminate the listener.
  future
    .onDiscard(lambda::bind(
        static_cast<void (*)(const UPID&, bool)>(terminate),
        listener->self(),
        true))
    .onAny(lambda::bind(
        static_cast<void (*)(const UPID&, bool)>(terminate),
        listener->self(),
        true));

  return future;
}

} // namespace event {


namespace internal {

namespace freezer {

Try<string> state(const string& hierarchy, const string& cgroup)
{
  Try<string> state = cgroups::read(hierarchy, cgroup, "freezer.state");

  if (state.isError()) {
    return Error("Failed to read freezer state: " + state.error());
  }

  return strings::trim(state.get());
}


Try<Nothing> state(
    const string& hierarchy,
    const string& cgroup,
    const string& state)
{
  if (state != "FROZEN" && state != "THAWED") {
    return Error("Invalid freezer state requested: " + state);
  }

  Try<Nothing> write = cgroups::write(
      hierarchy, cgroup, "freezer.state", state);
  if (write.isError()) {
    return Error("Failed to write '" + state +
                 "' to control 'freezer.state': " + write.error());
  } else {
    return Nothing();
  }
}

} // namespace freezer {

class Freezer : public Process<Freezer>
{
public:
  Freezer(
      const string& _hierarchy,
      const string& _cgroup)
    : ProcessBase(ID::generate("cgroups-freezer")),
      hierarchy(_hierarchy),
      cgroup(_cgroup),
      start(Clock::now()) {}

  ~Freezer() override {}

  void freeze()
  {
    Try<Nothing> freeze =
      internal::freezer::state(hierarchy, cgroup, "FROZEN");
    if (freeze.isError()) {
      promise.fail(freeze.error());
      terminate(self());
      return;
    }

    Try<string> state = internal::freezer::state(hierarchy, cgroup);
    if (state.isError()) {
      promise.fail(state.error());
      terminate(self());
      return;
    }

    if (state.get() == "FROZEN") {
      LOG(INFO) << "Successfully froze cgroup "
                << path::join(hierarchy, cgroup)
                << " after " << (Clock::now() - start);
      promise.set(Nothing());
      terminate(self());
      return;
    }

    // Attempt to freeze the freezer cgroup again.
    delay(Milliseconds(100), self(), &Self::freeze);
  }

  void thaw()
  {
    Try<Nothing> thaw = internal::freezer::state(hierarchy, cgroup, "THAWED");
    if (thaw.isError()) {
      promise.fail(thaw.error());
      terminate(self());
      return;
    }

    Try<string> state = internal::freezer::state(hierarchy, cgroup);
    if (state.isError()) {
      promise.fail(state.error());
      terminate(self());
      return;
    }

    if (state.get() == "THAWED") {
      LOG(INFO) << "Successfully thawed cgroup "
                << path::join(hierarchy, cgroup)
                << " after " << (Clock::now() - start);
      promise.set(Nothing());
      terminate(self());
      return;
    }

    // Attempt to thaw the freezer cgroup again.
    delay(Milliseconds(100), self(), &Self::thaw);
  }

  Future<Nothing> future() { return promise.future(); }

protected:
  void initialize() override
  {
    // Stop attempting to freeze/thaw if nobody cares.
    promise.future().onDiscard(lambda::bind(
          static_cast<void(*)(const UPID&, bool)>(terminate), self(), true));
  }

  void finalize() override
  {
    promise.discard();
  }

private:
  const string hierarchy;
  const string cgroup;
  const Time start;
  Promise<Nothing> promise;
};


// The process used to atomically kill all tasks in a cgroup.
class TasksKiller : public Process<TasksKiller>
{
public:
  TasksKiller(const string& _hierarchy, const string& _cgroup)
    : ProcessBase(ID::generate("cgroups-tasks-killer")),
      hierarchy(_hierarchy),
      cgroup(_cgroup) {}

  ~TasksKiller() override {}

  // Return a future indicating the state of the killer.
  // Failure occurs if any process in the cgroup is unable to be
  // killed.
  // Discarding the future will cause this process to stop the next time it
  // calls `freeze`: we don't want to stop at an arbitrary point since it might
  // leave the cgroup frozen.
  Future<Nothing> future() { return promise.future(); }

protected:
  void initialize() override
  {
    killTasks();
  }

  void finalize() override
  {
    chain.discard();

    // TODO(jieyu): Wait until 'chain' is in DISCARDED state before
    // discarding 'promise'.
    promise.discard();
  }

private:
  static Future<Nothing> freezeTimedout(
      Future<Nothing> future,
      const PID<TasksKiller>& pid)
  {
    // Cancel the freeze operation.
    future.discard();

    // Wait until the freeze is cancelled, and then attempt to kill the
    // processes before we thaw again, due to a bug in the kernel. See
    // MESOS-1758 for more details.  We thaw the cgroup before trying to freeze
    // again to allow any pending signals to be delivered. See MESOS-1689 for
    // details.  This is a short term hack until we have PID namespace support.
    return future
      .recover([](const Future<Nothing>&){ return Future<Nothing>(Nothing()); })
      .then(defer(pid, &Self::kill))
      .then(defer(pid, &Self::thaw))
      .then(defer(pid, &Self::freeze));
  }

  void killTasks() {
    // Chain together the steps needed to kill all tasks in the cgroup.
    chain = freeze()                     // Freeze the cgroup.
      .then(defer(self(), &Self::kill))  // Send kill signal.
      .then(defer(self(), &Self::thaw))  // Thaw cgroup to deliver signal.
      .then(defer(self(), &Self::reap)); // Wait until all pids are reaped.

    chain.onAny(defer(self(), &Self::finished, lambda::_1));
  }

  Future<Nothing> freeze()
  {
    // Don't start another `killTasks` cycle if we've been asked to stop.
    if (promise.future().hasDiscard()) {
        terminate(self());
        return Nothing();
    }

    // TODO(jieyu): This is a workaround for MESOS-1689. We will move
    // away from freezer once we have pid namespace support.
    return cgroups::freezer::freeze(hierarchy, cgroup).after(
        FREEZE_RETRY_INTERVAL,
        lambda::bind(&freezeTimedout, lambda::_1, self()));
  }

  Future<Nothing> kill()
  {
    Try<set<pid_t>> processes = cgroups::processes(hierarchy, cgroup);
    if (processes.isError()) {
      return Failure(processes.error());
    }

    // Reaping the frozen pids before we kill (and thaw) ensures we reap the
    // correct pids.
    foreach (const pid_t pid, processes.get()) {
      statuses.push_back(process::reap(pid));
    }

    Try<Nothing> kill = cgroups::kill(hierarchy, cgroup, SIGKILL);
    if (kill.isError()) {
      return Failure(kill.error());
    }

    return Nothing();
  }

  Future<Nothing> thaw()
  {
    return cgroups::freezer::thaw(hierarchy, cgroup);
  }

  Future<vector<Option<int>>> reap()
  {
    // Wait until we've reaped all processes.
    return collect(statuses);
  }

  void finished(const Future<vector<Option<int>>>& future)
  {
    if (future.isDiscarded()) {
      promise.fail("Unexpected discard of future");
      terminate(self());
      return;
    } else if (future.isFailed()) {
      // If the `cgroup` still exists in the hierarchy, treat this as
      // an error; otherwise, treat this as a success since the `cgroup`
      // has actually been cleaned up.
      if (os::exists(path::join(hierarchy, cgroup))) {
        promise.fail(future.failure());
      } else {
        promise.set(Nothing());
      }

      terminate(self());
      return;
    }

    // Verify the cgroup is now empty.
    Try<set<pid_t>> processes = cgroups::processes(hierarchy, cgroup);

    // If the `cgroup` is already removed, treat this as a success.
    if ((processes.isError() || !processes->empty()) &&
        os::exists(path::join(hierarchy, cgroup))) {
      promise.fail("Failed to kill all processes in cgroup: " +
                   (processes.isError() ? processes.error()
                                        : "processes remain"));
      terminate(self());
      return;
    }

    promise.set(Nothing());
    terminate(self());
  }

  const string hierarchy;
  const string cgroup;
  Promise<Nothing> promise;
  vector<Future<Option<int>>> statuses; // List of statuses for processes.
  Future<vector<Option<int>>> chain; // Used to discard all operations.
};


// The process used to destroy a cgroup.
class Destroyer : public Process<Destroyer>
{
public:
  Destroyer(const string& _hierarchy, const vector<string>& _cgroups)
    : ProcessBase(ID::generate("cgroups-destroyer")),
      hierarchy(_hierarchy),
      cgroups(_cgroups) {}

  ~Destroyer() override {}

  // Return a future indicating the state of the destroyer.
  // Failure occurs if any cgroup fails to be destroyed.
  Future<Nothing> future() { return promise.future(); }

protected:
  void initialize() override
  {
    // Stop when no one cares.
    promise.future().onDiscard(lambda::bind(
        static_cast<void (*)(const UPID&, bool)>(terminate), self(), true));

    // Kill tasks in the given cgroups in parallel. Use collect mechanism to
    // wait until all kill processes finish.
    foreach (const string& cgroup, cgroups) {
      internal::TasksKiller* killer =
        new internal::TasksKiller(hierarchy, cgroup);
      killers.push_back(killer->future());
      spawn(killer, true);
    }

    collect(killers)
      .onAny(defer(self(), &Destroyer::killed, lambda::_1));
  }

  void finalize() override
  {
    remover.discard();
    discard(killers);
    promise.discard();
  }

private:
  void killed(const Future<vector<Nothing>>& kill)
  {
    if (kill.isReady()) {
      remover = internal::remove(hierarchy, cgroups);
      remover.onAny(defer(self(), &Destroyer::removed, lambda::_1));
    } else if (kill.isDiscarded()) {
      promise.discard();
      terminate(self());
    } else if (kill.isFailed()) {
      promise.fail("Failed to kill tasks in nested cgroups: " +
                   kill.failure());
      terminate(self());
    }
  }

  void removed(const Future<Nothing>& removeAll)
  {
    if (removeAll.isReady()) {
      promise.set(Nothing());
    } else if (removeAll.isDiscarded()) {
      promise.discard();
    } else if (removeAll.isFailed()) {
      promise.fail("Failed to remove cgroups: " + removeAll.failure());
    }

    terminate(self());
  }

  const string hierarchy;
  const vector<string> cgroups;
  Promise<Nothing> promise;

  // The killer processes used to atomically kill tasks in each cgroup.
  vector<Future<Nothing>> killers;

  // Future used to destroy the cgroups once the tasks have been killed.
  Future<Nothing> remover;
};

} // namespace internal {


Future<Nothing> destroy(const string& hierarchy, const string& cgroup)
{
  // Construct the vector of cgroups to destroy.
  Try<vector<string>> cgroups = cgroups::get(hierarchy, cgroup);
  if (cgroups.isError()) {
    return Failure(
        "Failed to get nested cgroups: " + cgroups.error());
  }

  vector<string> candidates = cgroups.get();
  if (cgroup != "/") {
    candidates.push_back(cgroup);
  }

  if (candidates.empty()) {
    return Nothing();
  }

  // If the freezer subsystem is available, destroy the cgroups.
  if (cgroups::exists(hierarchy, cgroup, "freezer.state")) {
    internal::Destroyer* destroyer =
      new internal::Destroyer(hierarchy, candidates);
    Future<Nothing> future = destroyer->future();
    spawn(destroyer, true);
    return future;
  }

  // Attempt to remove the cgroups in a bottom-up fashion.
  return internal::remove(hierarchy, candidates);
}


static void __destroy(
    const Future<Nothing>& future,
    const Owned<Promise<Nothing>>& promise,
    const Duration& timeout)
{
  if (future.isReady()) {
    promise->set(future.get());
  } else if (future.isFailed()) {
    promise->fail(future.failure());
  } else {
    promise->fail("Timed out after " + stringify(timeout));
  }
}


static Future<Nothing> _destroy(
    Future<Nothing> future,
    const Duration& timeout)
{
  Owned<Promise<Nothing>> promise(new Promise<Nothing>());
  Future<Nothing> _future = promise->future();

  future.discard();
  future.onAny(lambda::bind(&__destroy, lambda::_1, promise, timeout));

  return _future;
}


Future<Nothing> destroy(
    const string& hierarchy,
    const string& cgroup,
    const Duration& timeout)
{
  return destroy(hierarchy, cgroup)
    .after(timeout, lambda::bind(&_destroy, lambda::_1, timeout));
}


// Forward declaration.
Future<bool> _cleanup(const string& hierarchy);


Future<bool> cleanup(const string& hierarchy)
{
  Try<bool> mounted = cgroups::mounted(hierarchy);
  if (mounted.isError()) {
    return Failure(mounted.error());
  }

  if (mounted.get()) {
    // Destroy all cgroups and then cleanup.
    return destroy(hierarchy)
      .then(lambda::bind(_cleanup, hierarchy));
  } else {
    // Remove the directory if it still exists.
    if (os::exists(hierarchy)) {
      Try<Nothing> rmdir = os::rmdir(hierarchy);
      if (rmdir.isError()) {
        return Failure(rmdir.error());
      }
    }
  }

  return true;
}


Future<bool> _cleanup(const string& hierarchy)
{
  // Remove the hierarchy.
  Try<Nothing> unmount = cgroups::unmount(hierarchy);
  if (unmount.isError()) {
    return Failure(unmount.error());
  }

  // Remove the directory if it still exists.
  if (os::exists(hierarchy)) {
    Try<Nothing> rmdir = os::rmdir(hierarchy);
    if (rmdir.isError()) {
      return Failure(rmdir.error());
    }
  }

  return true;
}


Try<hashmap<string, uint64_t>> stat(
    const string& hierarchy,
    const string& cgroup,
    const string& file)
{
  Try<string> contents = cgroups::read(hierarchy, cgroup, file);

  if (contents.isError()) {
    return Error(contents.error());
  }

  hashmap<string, uint64_t> result;

  foreach (const string& line, strings::split(contents.get(), "\n")) {
    // Skip empty lines.
    if (strings::trim(line).empty()) {
      continue;
    }

    string name;
    uint64_t value;

    // Expected line format: "%s %llu".
    istringstream stream(line);
    stream >> name >> value;

    if (stream.fail()) {
      return Error("Unexpected line format in " + file + ": " + line);
    }

    result[name] = value;
  }

  return result;
}


namespace internal {

// Helper for finding the cgroup of the specified pid for the
// specified subsystem.
Result<string> cgroup(pid_t pid, const string& subsystem)
{
  // Determine cgroup for hierarchy with the subsystem attached.
  string path = path::join("/proc", stringify(pid), "cgroup");

  Try<string> read = os::read(path);

  if (read.isError()) {
    return Error("Failed to read " + path + ": " + read.error());
  }

  // Now determine the cgroup by parsing each line of the output which
  // should be of the form "N:subsystems:cgroup" where 'N' is the
  // hierarchy number and 'subsystems' are the attached subsystems and
  // 'cgroup' is the relative path to the cgroup from the hierarchy
  // path.
  Option<string> cgroup = None();

  foreach (const string& line, strings::tokenize(read.get(), "\n")) {
    vector<string> tokens = strings::tokenize(line, ":");

    // The second field is empty for cgroups v2 hierarchy.
    if (tokens.size() == 2) {
      continue;
    } else if (tokens.size() != 3) {
      return Error("Unexpected format in " + path);
    }

    foreach (const string& token, strings::tokenize(tokens[1], ",")) {
      if (subsystem == token) {
        cgroup = tokens[2];
      }
    }
  }

  return cgroup;
}

} // namespace internal {


namespace blkio {

Result<string> cgroup(pid_t pid)
{
  return internal::cgroup(pid, "blkio");
}


unsigned int Device::getMajor() const
{
  return major(value);
}


unsigned int Device::getMinor() const
{
  return minor(value);
}


Try<Device> Device::parse(const string& s)
{
  vector<string> device = strings::tokenize(s, ":");
  if (device.size() != 2) {
    return Error("Invalid major:minor device number: '" + s + "'");
  }

  Try<unsigned int> major = numify<unsigned int>(device[0]);
  if (major.isError()) {
    return Error("Invalid device major number: '" + device[0] + "'");
  }

  Try<unsigned int> minor = numify<unsigned int>(device[1]);
  if (minor.isError()) {
    return Error("Invalid device minor number: '" + device[1] + "'");
  }

  return Device(makedev(major.get(), minor.get()));
}


static bool isOperation(const string& s)
{
  return (s == "Total" ||
          s == "Read" ||
          s == "Write" ||
          s == "Sync" ||
          s == "Async" ||
          s == "Discard");
}


static Try<Operation> parseOperation(const string& s)
{
  if (s == "Total") {
    return Operation::TOTAL;
  } else if (s == "Read") {
    return Operation::READ;
  } else if (s == "Write") {
    return Operation::WRITE;
  } else if (s == "Sync") {
    return Operation::SYNC;
  } else if (s == "Async") {
    return Operation::ASYNC;
  } else if (s == "Discard") {
    return Operation::DISCARD;
  }

  return Error("Invalid Operation value: '" + s + "'");
}


Try<Value> Value::parse(const string& s)
{
  vector<string> tokens = strings::tokenize(s, " ");
  if (tokens.size() == 1) {
    Try<uint64_t> value = numify<uint64_t>(tokens[0]);
    if (value.isError()) {
      return Error("Value is not a number: '" + tokens[0] + "'");
    }

    return Value{None(), None(), value.get()};
  }

  Option<Device> device;
  int offset = 0;

  if (tokens.size() == 3) {
    Try<Device> dev = Device::parse(tokens[0]);
    if (dev.isError()) {
      return Error(dev.error());
    }

    device = dev.get();
    offset++;
  } else if (tokens.size() != 2) {
    return Error("Invalid blkio value: '" + s + "'");
  }

  if (!isOperation(tokens[offset])) {
    Try<Device> dev = Device::parse(tokens[offset]);
    if (dev.isError()) {
      return Error(dev.error());
    }

    Try<uint64_t> value = numify<uint64_t>(tokens[offset + 1]);
    if (value.isError()) {
      return Error("Value is not a number: '" + tokens[offset + 1] + "'");
    }

    return Value{dev.get(), None(), value.get()};
  }

  Try<Operation> operation = parseOperation(tokens[offset]);
  if (operation.isError()) {
    return Error(operation.error());
  }

  Try<uint64_t> value = numify<uint64_t>(tokens[offset + 1]);
  if (value.isError()) {
    return Error("Value is not a number: " + value.error());
  }

  return Value{device, operation.get(), value.get()};
}


static Try<vector<Value>> readEntries(
    const string& hierarchy,
    const string& cgroup,
    const string& control)
{
  Try<string> read = cgroups::read(hierarchy, cgroup, control);
  if (read.isError()) {
    return Error("Failed to read from '" + control + "': " + read.error());
  }

  vector<Value> entries;

  foreach (const string& s, strings::tokenize(read.get(), "\n")) {
    Try<Value> value = Value::parse(s);
    if (value.isError()) {
      return Error("Failed to parse blkio value '" + s + "' from '" +
                   control + "': " + value.error());
    }

    entries.push_back(value.get());
  }

  return entries;
}


namespace cfq {

Try<vector<Value>> time(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.time");
}


Try<vector<Value>> time_recursive(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.time_recursive");
}


Try<vector<Value>> sectors(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.sectors");
}


Try<vector<Value>> sectors_recursive(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.sectors_recursive");
}


Try<vector<Value>> io_merged(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.io_merged");
}


Try<vector<Value>> io_merged_recursive(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.io_merged_recursive");
}


Try<vector<Value>> io_queued(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.io_queued");
}


Try<vector<Value>> io_queued_recursive(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.io_queued_recursive");
}


Try<vector<Value>> io_service_bytes(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.io_service_bytes");
}


Try<vector<Value>> io_service_bytes_recursive(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.io_service_bytes_recursive");
}


Try<vector<Value>> io_service_time(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.io_service_time");
}


Try<vector<Value>> io_service_time_recursive(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.io_service_time_recursive");
}


Try<vector<Value>> io_serviced(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.io_serviced");
}


Try<vector<Value>> io_serviced_recursive(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.io_serviced_recursive");
}


Try<vector<Value>> io_wait_time(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.io_wait_time");
}


Try<vector<Value>> io_wait_time_recursive(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.io_wait_time_recursive");
}

} // namespace cfq {


namespace throttle {

Try<vector<Value>> io_service_bytes(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.throttle.io_service_bytes");
}


Try<vector<Value>> io_serviced(
    const string& hierarchy,
    const string& cgroup)
{
  return readEntries(
      hierarchy,
      cgroup,
      "blkio.throttle.io_serviced");
}

} // namespace throttle {
} // namespace blkio {


namespace cpu {

Result<string> cgroup(pid_t pid)
{
  return internal::cgroup(pid, "cpu");
}


Try<Nothing> shares(
    const string& hierarchy,
    const string& cgroup,
    uint64_t shares)
{
  return cgroups::write(
      hierarchy,
      cgroup,
      "cpu.shares",
      stringify(shares));
}


Try<uint64_t> shares(
    const string& hierarchy,
    const string& cgroup)
{
  Try<string> read = cgroups::read(hierarchy, cgroup, "cpu.shares");

  if (read.isError()) {
    return Error(read.error());
  }

  uint64_t shares;
  istringstream ss(read.get());

  ss >> shares;

  return shares;
}


Try<Nothing> cfs_period_us(
    const string& hierarchy,
    const string& cgroup,
    const Duration& duration)
{
  return cgroups::write(
      hierarchy,
      cgroup,
      "cpu.cfs_period_us",
      stringify(static_cast<uint64_t>(duration.us())));
}


Try<Duration> cfs_quota_us(
    const string& hierarchy,
    const string& cgroup)
{
  Try<string> read = cgroups::read(hierarchy, cgroup, "cpu.cfs_quota_us");

  if (read.isError()) {
    return Error(read.error());
  }

  return Duration::parse(strings::trim(read.get()) + "us");
}


Try<Nothing> cfs_quota_us(
    const string& hierarchy,
    const string& cgroup,
    const Duration& duration)
{
  return cgroups::write(
      hierarchy,
      cgroup,
      "cpu.cfs_quota_us",
      stringify(static_cast<int64_t>(duration.us())));
}

} // namespace cpu {

namespace cpuacct {

Result<string> cgroup(pid_t pid)
{
  return internal::cgroup(pid, "cpuacct");
}


Try<Stats> stat(
    const string& hierarchy,
    const string& cgroup)
{
  const Try<hashmap<string, uint64_t>> stats =
    cgroups::stat(hierarchy, cgroup, "cpuacct.stat");

  if (!stats.isSome()) {
    return Error(stats.error());
  }

  if (!stats->contains("user") || !stats->contains("system")) {
    return Error("Failed to get user/system value from cpuacct.stat");
  }

  // Get user ticks per second. This value is constant for the lifetime of a
  // process.
  // TODO(Jojy): Move system constants to a separate compilation unit.
  static long userTicks = sysconf(_SC_CLK_TCK);
  if (userTicks <= 0) {
    return ErrnoError("Failed to get _SC_CLK_TCK");
  }

  Try<Duration> user = Duration::create((double)stats->at("user") / userTicks);

  if (user.isError()) {
    return Error(
        "Failed to convert user ticks to Duration: " + user.error());
  }

  Try<Duration> system =
    Duration::create((double)stats->at("system") / userTicks);

  if (system.isError()) {
    return Error(
        "Failed to convert system ticks to Duration: " + system.error());
  }

  return Stats({user.get(), system.get()});
}

} // namespace cpuacct {

namespace memory {

Result<string> cgroup(pid_t pid)
{
  return internal::cgroup(pid, "memory");
}


Try<Bytes> limit_in_bytes(const string& hierarchy, const string& cgroup)
{
  Try<string> read = cgroups::read(
      hierarchy, cgroup, "memory.limit_in_bytes");

  if (read.isError()) {
    return Error(read.error());
  }

  return Bytes::parse(strings::trim(read.get()) + "B");
}


Try<Nothing> limit_in_bytes(
    const string& hierarchy,
    const string& cgroup,
    const Bytes& limit)
{
  return cgroups::write(
      hierarchy,
      cgroup,
      "memory.limit_in_bytes",
      stringify(limit.bytes()));
}


Result<Bytes> memsw_limit_in_bytes(
    const string& hierarchy,
    const string& cgroup)
{
  if (!cgroups::exists(hierarchy, cgroup, "memory.memsw.limit_in_bytes")) {
    return None();
  }

  Try<string> read = cgroups::read(
      hierarchy, cgroup, "memory.memsw.limit_in_bytes");

  if (read.isError()) {
    return Error(read.error());
  }

  Try<Bytes> bytes = Bytes::parse(strings::trim(read.get()) + "B");

  if (bytes.isError()) {
    return Error(bytes.error());
  }

  return bytes.get();
}


Try<bool> memsw_limit_in_bytes(
    const string& hierarchy,
    const string& cgroup,
    const Bytes& limit)
{
  if (!cgroups::exists(hierarchy, cgroup, "memory.memsw.limit_in_bytes")) {
    return false;
  }

  Try<Nothing> write = cgroups::write(
      hierarchy,
      cgroup,
      "memory.memsw.limit_in_bytes",
      stringify(limit.bytes()));

  if (write.isError()) {
    return Error(write.error());
  }

  return true;
}


Try<Bytes> soft_limit_in_bytes(const string& hierarchy, const string& cgroup)
{
  Try<string> read = cgroups::read(
      hierarchy, cgroup, "memory.soft_limit_in_bytes");

  if (read.isError()) {
    return Error(read.error());
  }

  return Bytes::parse(strings::trim(read.get()) + "B");
}


Try<Nothing> soft_limit_in_bytes(
    const string& hierarchy,
    const string& cgroup,
    const Bytes& limit)
{
  return cgroups::write(
      hierarchy,
      cgroup,
      "memory.soft_limit_in_bytes",
      stringify(limit.bytes()));
}


Try<Bytes> usage_in_bytes(const string& hierarchy, const string& cgroup)
{
  Try<string> read = cgroups::read(
      hierarchy, cgroup, "memory.usage_in_bytes");

  if (read.isError()) {
    return Error(read.error());
  }

  return Bytes::parse(strings::trim(read.get()) + "B");
}


Try<Bytes> memsw_usage_in_bytes(const string& hierarchy, const string& cgroup)
{
  Try<string> read = cgroups::read(
      hierarchy, cgroup, "memory.memsw.usage_in_bytes");

  if (read.isError()) {
    return Error(read.error());
  }

  return Bytes::parse(strings::trim(read.get()) + "B");
}


Try<Bytes> max_usage_in_bytes(const string& hierarchy, const string& cgroup)
{
  Try<string> read = cgroups::read(
      hierarchy, cgroup, "memory.max_usage_in_bytes");

  if (read.isError()) {
    return Error(read.error());
  }

  return Bytes::parse(strings::trim(read.get()) + "B");
}


namespace oom {

Future<Nothing> listen(const string& hierarchy, const string& cgroup)
{
  return cgroups::event::listen(hierarchy, cgroup, "memory.oom_control")
    .then([]() { return Nothing(); });
}


namespace killer {

Try<bool> enabled(const string& hierarchy, const string& cgroup)
{
  if (!cgroups::exists(hierarchy, cgroup, "memory.oom_control")) {
    return Error("Could not find 'memory.oom_control' control file");
  }

  Try<string> read = cgroups::read(hierarchy, cgroup, "memory.oom_control");
  if (read.isError()) {
    return Error("Could not read 'memory.oom_control' control file: " +
                 read.error());
  }

  map<string, vector<string>> pairs = strings::pairs(read.get(), "\n", " ");

  if (pairs.count("oom_kill_disable") != 1 ||
      pairs["oom_kill_disable"].size() != 1) {
    return Error("Could not determine oom control state");
  }

  // Enabled if not disabled.
  return pairs["oom_kill_disable"].front() == "0";
}


Try<Nothing> enable(const string& hierarchy, const string& cgroup)
{
  Try<bool> enabled = killer::enabled(hierarchy, cgroup);

  if (enabled.isError()) {
    return Error(enabled.error());
  }

  if (!enabled.get()) {
    Try<Nothing> write = cgroups::write(
        hierarchy, cgroup, "memory.oom_control", "0");

    if (write.isError()) {
      return Error("Could not write 'memory.oom_control' control file: " +
                   write.error());
    }
  }

  return Nothing();
}


Try<Nothing> disable(const string& hierarchy, const string& cgroup)
{
  Try<bool> enabled = killer::enabled(hierarchy, cgroup);

  if (enabled.isError()) {
    return Error(enabled.error());
  }

  if (enabled.get()) {
    Try<Nothing> write = cgroups::write(
        hierarchy, cgroup, "memory.oom_control", "1");

    if (write.isError()) {
      return Error("Could not write 'memory.oom_control' control file: " +
                   write.error());
    }
  }

  return Nothing();
}

} // namespace killer {

} // namespace oom {


namespace pressure {

ostream& operator<<(ostream& stream, Level level)
{
  switch (level) {
    case LOW:      return stream << "low";
    case MEDIUM:   return stream << "medium";
    case CRITICAL: return stream << "critical";
    // We omit the default case because we assume -Wswitch
    // will trigger a compile-time error if a case is missed.
  }

  UNREACHABLE();
}


// The process drives the event::Listener to keep listening on cgroups
// memory pressure counters.
class CounterProcess : public Process<CounterProcess>
{
public:
  CounterProcess(const string& hierarchy,
                 const string& cgroup,
                 Level level)
    : ProcessBase(ID::generate("cgroups-counter")),
      value_(0),
      error(None()),
      process(new event::Listener(
          hierarchy,
          cgroup,
          "memory.pressure_level",
          stringify(level))) {}

  ~CounterProcess() override {}

  Future<uint64_t> value()
  {
    if (error.isSome()) {
      return Failure(error.get());
    }

    return value_;
  }

protected:
  void initialize() override
  {
    spawn(CHECK_NOTNULL(process.get()));
    listen();
  }

  void finalize() override
  {
    terminate(process.get());
    wait(process.get());
  }

private:
  void listen()
  {
    dispatch(process.get(), &event::Listener::listen)
      .onAny(defer(self(), &CounterProcess::_listen, lambda::_1));
  }

  void _listen(const process::Future<uint64_t>& future)
  {
    CHECK_NONE(error);

    if (future.isReady()) {
      value_ += future.get();
      listen();
    } else if (future.isFailed()) {
      error = Error(future.failure());
    } else if (future.isDiscarded()) {
      error = Error("Listening stopped unexpectedly");
    }
  }

  uint64_t value_;
  Option<Error> error;
  process::Owned<event::Listener> process;
};


Try<Owned<Counter>> Counter::create(
    const string& hierarchy,
    const string& cgroup,
    Level level)
{
  return Owned<Counter>(new Counter(hierarchy, cgroup, level));
}


Counter::Counter(const string& hierarchy,
                 const string& cgroup,
                 Level level)
  : process(new CounterProcess(hierarchy, cgroup, level))
{
  spawn(CHECK_NOTNULL(process.get()));
}


Counter::~Counter()
{
  terminate(process.get(), true);
  wait(process.get());
}


Future<uint64_t> Counter::value() const
{
  return dispatch(process.get(), &CounterProcess::value);
}

} // namespace pressure {

} // namespace memory {


namespace devices {

ostream& operator<<(ostream& stream, const Entry::Selector::Type& type)
{
  switch (type) {
    case Entry::Selector::Type::ALL:       return stream << "a";
    case Entry::Selector::Type::BLOCK:     return stream << "b";
    case Entry::Selector::Type::CHARACTER: return stream << "c";
    // We omit the default case because we assume -Wswitch
    // will trigger a compile-time error if a case is missed.
  }

  UNREACHABLE();
}


ostream& operator<<(ostream& stream, const Entry::Selector& selector)
{
  stream << selector.type << " ";

  if (selector.major.isSome()) {
    stream << stringify(selector.major.get());
  } else {
    stream << "*";
  }

  stream << ":";

  if (selector.minor.isSome()) {
    stream << stringify(selector.minor.get());
  } else {
    stream << "*";
  }

  return stream;
}


ostream& operator<<(ostream& stream, const Entry::Access& access)
{
  if (access.read) {
    stream << "r";
  }
  if (access.write) {
    stream << "w";
  }
  if (access.mknod) {
    stream << "m";
  }

  return stream;
}


ostream& operator<<(ostream& stream, const Entry& entry)
{
  return stream << entry.selector << " " << entry.access;
}


bool operator==(const Entry::Selector& left, const Entry::Selector& right)
{
  return left.type == right.type &&
         left.minor == right.minor &&
         left.major == right.major;
}


bool operator==(const Entry::Access& left, const Entry::Access& right)
{
  return left.read == right.read &&
         left.write == right.write &&
         left.mknod == right.mknod;
}


bool operator==(const Entry& left, const Entry& right)
{
  return left.selector == right.selector &&
         left.access == right.access;
}


Try<Entry> Entry::parse(const string& s)
{
  vector<string> tokens = strings::tokenize(s, " ");

  if (tokens.empty()) {
    return Error("Invalid format");
  }

  Entry entry;

  // Parse the device type.
  // By default, when "a" is set as the device type
  // all other fields in the device entry are ignored!
  // (i.e. "a" implies a *:* rwm").
  if (tokens[0] == "a") {
    entry.selector.type = Selector::Type::ALL;
    entry.selector.major = None();
    entry.selector.minor = None();
    entry.access.read = true;
    entry.access.write = true;
    entry.access.mknod = true;
    return entry;
  }

  if (tokens.size() != 3) {
    return Error("Invalid format");
  }

  // Other than for the "a" device, a correctly formed entry must
  // contain exactly 3 space-separated tokens of the form:
  // "{b,c} <major>:<minor> [r][w][m]".
  if (tokens[0] == "b") {
    entry.selector.type = Selector::Type::BLOCK;
  } else if (tokens[0] == "c") {
    entry.selector.type = Selector::Type::CHARACTER;
  } else {
    return Error("Invalid format");
  }

  // Parse the device major/minor numbers.
  vector<string> deviceNumbers = strings::tokenize(tokens[1], ":");

  if (deviceNumbers.size() != 2) {
    return Error("Invalid format");
  }

  // The device major/minor numbers must either be "*"
  // or an unsigned integer. A value of None() means "*".
  entry.selector.major = None();
  entry.selector.minor = None();

  if (deviceNumbers[0] != "*") {
    Try<unsigned int> major = numify<unsigned int>(deviceNumbers[0]);
    if (major.isError()) {
      return Error("Invalid format");
    }

    entry.selector.major = major.get();
  }

  if (deviceNumbers[1] != "*") {
    Try<unsigned int> minor = numify<unsigned int>(deviceNumbers[1]);
    if (minor.isError()) {
      return Error("Invalid format");
    }

    entry.selector.minor = minor.get();
  }

  // Parse the access bits.
  string permissions = tokens[2];

  if (permissions.size() > 3) {
    return Error("Invalid format");
  }

  entry.access.read = false;
  entry.access.write = false;
  entry.access.mknod = false;

  foreach (char permission, permissions) {
    if (permission == 'r') {
      entry.access.read = true;
    } else if (permission == 'w') {
      entry.access.write = true;
    } else if (permission == 'm') {
      entry.access.mknod = true;
    } else {
      return Error("Invalid format");
    }
  }

  return entry;
}


Try<vector<Entry>> list(
    const string& hierarchy,
    const string& cgroup)
{
  Try<string> read = cgroups::read(hierarchy, cgroup, "devices.list");

  if (read.isError()) {
    return Error("Failed to read from 'devices.list': " + read.error());
  }

  vector<Entry> entries;

  foreach (const string& s, strings::tokenize(read.get(), "\n")) {
    Try<Entry> entry = Entry::parse(s);

    if (entry.isError()) {
      return Error("Failed to parse device entry '" + s + "'"
                   " from 'devices.list': " + entry.error());
    }

    entries.push_back(entry.get());
  }

  return entries;
}


Try<Nothing> allow(
    const string& hierarchy,
    const string& cgroup,
    const Entry& entry)
{
  Try<Nothing> write = cgroups::write(
     hierarchy,
     cgroup,
     "devices.allow",
     stringify(entry));

  if (write.isError()) {
    return Error("Failed to write to 'devices.allow': " + write.error());
  }

  return Nothing();
}


Try<Nothing> deny(
    const string& hierarchy,
    const string& cgroup,
    const Entry& entry)
{
  Try<Nothing> write = cgroups::write(
     hierarchy,
     cgroup,
     "devices.deny",
     stringify(entry));

  if (write.isError()) {
    return Error("Failed to write to 'devices.deny': " + write.error());
  }

  return Nothing();
}


} // namespace devices {


namespace freezer {

Future<Nothing> freeze(
    const string& hierarchy,
    const string& cgroup)
{
  LOG(INFO) << "Freezing cgroup " << path::join(hierarchy, cgroup);

  internal::Freezer* freezer = new internal::Freezer(hierarchy, cgroup);
  PID<internal::Freezer> pid = freezer->self();

  Future<Nothing> future = freezer->future();
  spawn(freezer, true);

  dispatch(pid, &internal::Freezer::freeze);

  return future;
}


Future<Nothing> thaw(
    const string& hierarchy,
    const string& cgroup)
{
  LOG(INFO) << "Thawing cgroup " << path::join(hierarchy, cgroup);

  internal::Freezer* freezer = new internal::Freezer(hierarchy, cgroup);
  PID<internal::Freezer> pid = freezer->self();

  Future<Nothing> future = freezer->future();
  spawn(freezer, true);

  dispatch(pid, &internal::Freezer::thaw);

  return future;
}

} // namespace freezer {


namespace net_cls {

Try<uint32_t> classid(
    const string& hierarchy,
    const string& cgroup)
{
  Try<string> read = cgroups::read(hierarchy, cgroup, "net_cls.classid");
  if (read.isError()) {
    return Error("Unable to read the `net_cls.classid`: " + read.error());
  }

  Try<uint32_t> handle = numify<uint32_t>(strings::trim(read.get()));
  if (handle.isError()) {
    return Error("Not a valid number");
  }

  return handle.get();
}


Try<Nothing> classid(
    const string& hierarchy,
    const string& cgroup,
    uint32_t handle)
{
  Try<Nothing> write = cgroups::write(
      hierarchy,
      cgroup,
      "net_cls.classid",
      stringify(handle));

  if (write.isError()) {
    return Error("Failed to write to 'net_cls.classid': " + write.error());
  }

  return Nothing();
}

} // namespace net_cls {


namespace named {

Result<string> cgroup(const string& hierarchyName, pid_t pid)
{
  return internal::cgroup(pid, "name=" + hierarchyName);
}

} // namespace named {

} // namespace cgroups {
