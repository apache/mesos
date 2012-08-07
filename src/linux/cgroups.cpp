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

#include <errno.h>
#include <fts.h>
#include <signal.h>
#include <unistd.h>

#include <sys/syscall.h>
#include <sys/types.h>

// For older versions of glibc we need to get O_CLOEXEC from
// linux/fcntl.h, but on some versions that requires sys/types.h to be
// included _first_.
#include <linux/fcntl.h>

#include <glog/logging.h>

#include <fstream>
#include <map>
#include <sstream>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/io.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "linux/cgroups.hpp"
#include "linux/fs.hpp"

using namespace process;
using namespace mesos::internal;

namespace cgroups {

namespace internal {


// Snapshot a subsystem (modeled after a line in /proc/cgroups).
struct SubsystemInfo
{
  SubsystemInfo()
    : hierarchy(0),
      cgroups(0),
      enabled(false) {}

  SubsystemInfo(const std::string& _name,
                int _hierarchy,
                int _cgroups,
                bool _enabled)
    : name(_name),
      hierarchy(_hierarchy),
      cgroups(_cgroups),
      enabled(_enabled) {}

  std::string name; // Name of the subsystem.
  int hierarchy;    // ID of the hierarchy the subsystem is attached to.
  int cgroups;      // Number of cgroups for the subsystem.
  bool enabled;     // Whether the subsystem is enabled or not.
};


// Return information about subsystems on the current machine. We get
// information from /proc/cgroups file. Each line in it describes a subsystem.
// @return  A map from subsystem names to SubsystemInfo instances if succeeds.
//          Error if any unexpected happens.
static Try<std::map<std::string, SubsystemInfo> > subsystems()
{
  std::ifstream file("/proc/cgroups");

  if (!file.is_open()) {
    return Try<std::map<std::string, SubsystemInfo> >::error(
        "Failed to open /proc/cgroups");
  }

  std::map<std::string, SubsystemInfo> infos;

  while (!file.eof()) {
    std::string line;
    std::getline(file, line);

    if (file.fail()) {
      if (!file.eof()) {
        file.close();
        return Try<std::map<std::string, SubsystemInfo> >::error(
            "Failed to read /proc/cgroups");
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
        std::string name;
        int hierarchy;
        int cgroups;
        bool enabled;

        std::istringstream ss(line);
        ss >> std::dec >> name >> hierarchy >> cgroups >> enabled;

        // Check for any read/parse errors.
        if (ss.fail() && !ss.eof()) {
          file.close();
          return Try<std::map<std::string, SubsystemInfo> >::error(
              "Failed to parse /proc/cgroups");
        }

        infos[name] = SubsystemInfo(name, hierarchy, cgroups, enabled);
      }
    }
  }

  file.close();
  return infos;
}


// Mount a cgroups virtual file system (with proper subsystems attached) to a
// given directory (hierarchy root). The cgroups virtual file system is the
// interface exposed by the kernel to control cgroups. Each directory created
// inside the hierarchy root is a cgroup. Therefore, cgroups are organized in a
// tree like structure. User can specify what subsystems to be attached to the
// hierarchy root so that these subsystems can be controlled through normal file
// system APIs. A subsystem can only be attached to one hierarchy. This function
// assumes the given hierarchy is an empty directory and the given subsystems
// are enabled in the current platform.
// @param   hierarchy   Path to the hierarchy root.
// @param   subsystems  Comma-separated subsystem names.
// @return  True if the operation succeeds.
//          Error if the operation fails.
static Try<bool> mount(const std::string& hierarchy,
                       const std::string& subsystems)
{
  Try<bool> result = fs::mount(subsystems,
                               hierarchy,
                               "cgroup",
                               0,
                               subsystems.c_str());
  if (result.isError()) {
    return Try<bool>::error(result.error());
  }

  return true;
}


// Unmount the cgroups virtual file system from the given hierarchy root. Make
// sure to remove all cgroups in the hierarchy before unmount. This function
// assumes the given hierarchy is currently mounted with a cgroups virtual file
// system.
// @param   hierarchy   Path to the hierarchy root.
// @return  True if the operation succeeds.
//          Error if the operation fails.
static Try<bool> unmount(const std::string& hierarchy)
{
  Try<bool> result = fs::unmount(hierarchy);
  if (result.isError()) {
    return Try<bool>::error(result.error());
  }

  return true;
}


// Create a cgroup in a given hierarchy. To create a cgroup, one just need to
// create a directory in the cgroups virtual file system. The given cgroup is a
// relative path to the given hierarchy. This function assumes the given
// hierarchy is valid and is currently mounted with a cgroup virtual file
// system. The function also assumes the given cgroup is valid. This function
// will not create directories recursively, which means it will return error if
// any of the parent cgroups does not exist.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  True if the operation succeeds.
//          Error if the operation fails.
static Try<bool> createCgroup(const std::string& hierarchy,
                              const std::string& cgroup)
{
  std::string path = hierarchy + "/" + cgroup;
  if (::mkdir(path.c_str(), 0755) < 0) {
    return Try<bool>::error(
        "Failed to create cgroup at " + path + ": " + strerror(errno));
  }

  return true;
}


// Remove a cgroup in a given hierarchy. To remove a cgroup, one needs to remove
// the corresponding directory in the cgroups virtual file system. A cgroup
// cannot be removed if it has processes or sub-cgroups inside. This function
// does nothing but tries to remove the corresponding directory of the given
// cgroup. It will return error if the remove operation fails because it has
// either processes or sub-cgroups inside.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  True if the operation succeeds.
//          Error if the operation fails.
static Try<bool> removeCgroup(const std::string& hierarchy,
                              const std::string& cgroup)
{
  std::string path = hierarchy + "/" + cgroup;
  if (::rmdir(path.c_str()) < 0) {
    return Try<bool>::error(
        "Failed to remove cgroup at " + path + ": " + strerror(errno));
  }

  return true;
}


// Read a control file. Control files are the gateway to monitor and control
// cgroups. This function assumes the cgroups virtual file systems are properly
// mounted on the given hierarchy, and the given cgroup has been already created
// properly. The given control file name should also be valid.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @return  The value read from the control file.
static Try<std::string> readControl(const std::string& hierarchy,
                                    const std::string& cgroup,
                                    const std::string& control)
{
  std::string path = hierarchy + "/" + cgroup + "/" + control;

  // We do not use os::read here because it cannot correctly read proc or
  // cgroups control files (lseek will return error).
  std::ifstream file(path.c_str());

  if (!file.is_open()) {
    return Try<std::string>::error("Failed to open file " + path);
  }

  std::ostringstream ss;
  ss << file.rdbuf();

  if (file.fail()) {
    // TODO(jieyu): Make sure that the way we get errno here is portable.
    std::string msg = strerror(errno);
    file.close();
    return Try<std::string>::error(msg);
  }

  file.close();
  return ss.str();
}


// Write a control file.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @param   value       Value to be written.
// @return  True if the operation succeeds.
//          Error if the operation fails.
static Try<bool> writeControl(const std::string& hierarchy,
                              const std::string& cgroup,
                              const std::string& control,
                              const std::string& value)
{
  std::string path = hierarchy + "/" + cgroup + "/" + control;
  std::ofstream file(path.c_str());

  if (!file.is_open()) {
    return Try<bool>::error("Failed to open file " + path);
  }

  file << value << std::endl;

  if (file.fail()) {
    // TODO(jieyu): Make sure that the way we get errno here is portable.
    std::string msg = strerror(errno);
    file.close();
    return Try<bool>::error(msg);
  }

  file.close();
  return true;
}

} // namespace internal {


bool enabled()
{
  return os::exists("/proc/cgroups");
}


Try<bool> enabled(const std::string& subsystems)
{
  std::vector<std::string> names = strings::split(subsystems, ",");
  if (names.empty()) {
    return Try<bool>::error("No subsystem is specified");
  }

  Try<std::map<std::string, internal::SubsystemInfo> >
    infosResult = internal::subsystems();
  if (infosResult.isError()) {
    return Try<bool>::error(infosResult.error());
  }

  std::map<std::string, internal::SubsystemInfo> infos = infosResult.get();
  bool disabled = false;  // Whether some subsystems are not enabled.

  foreach (const std::string& name, names) {
    if (infos.find(name) == infos.end()) {
      return Try<bool>::error("Subsystem " + name + " not found");
    }
    if (!infos[name].enabled) {
      // Here, we don't return false immediately because we want to return
      // error if any of the given subsystems contain an invalid name.
      disabled = true;
    }
  }

  return !disabled;
}


Try<bool> busy(const std::string& subsystems)
{
  std::vector<std::string> names = strings::split(subsystems, ",");
  if (names.empty()) {
    return Try<bool>::error("No subsystem is specified");
  }

  Try<std::map<std::string, internal::SubsystemInfo> >
    infosResult = internal::subsystems();
  if (infosResult.isError()) {
    return Try<bool>::error(infosResult.error());
  }

  std::map<std::string, internal::SubsystemInfo> infos = infosResult.get();
  bool busy = false;

  foreach (const std::string& name, names) {
    if (infos.find(name) == infos.end()) {
      return Try<bool>::error("Subsystem " + name + " not found");
    }
    if (infos[name].hierarchy != 0) {
      // Here, we don't return false immediately because we want to return
      // error if any of the given subsystems contain an invalid name.
      busy = true;
    }
  }

  return busy;
}


Try<std::set<std::string> > subsystems()
{
  Try<std::map<std::string, internal::SubsystemInfo> >
    infos = internal::subsystems();
  if (infos.isError()) {
    return Try<std::set<std::string> >::error(infos.error());
  }

  std::set<std::string> names;
  foreachvalue (const internal::SubsystemInfo& info, infos.get()) {
    if (info.enabled) {
      names.insert(info.name);
    }
  }

  return names;
}


Try<std::set<std::string> > subsystems(const std::string& hierarchy)
{
  // We compare the canonicalized absolute paths.
  Try<std::string> hierarchyAbsPath = os::realpath(hierarchy);
  if (hierarchyAbsPath.isError()) {
    return Try<std::set<std::string> >::error(hierarchyAbsPath.error());
  }

  // Read currently mounted file systems from /proc/mounts.
  Try<fs::MountTable> table = fs::MountTable::read("/proc/mounts");
  if (table.isError()) {
    return Try<std::set<std::string> >::error(table.error());
  }

  // Check if hierarchy is a mount point of type cgroup.
  Option<fs::MountTable::Entry> hierarchyEntry;
  foreach (const fs::MountTable::Entry& entry, table.get().entries) {
    if (entry.type == "cgroup") {
      Try<std::string> dirAbsPath = os::realpath(entry.dir);
      if (dirAbsPath.isError()) {
        return Try<std::set<std::string> >::error(dirAbsPath.error());
      }

      // Seems that a directory can be mounted more than once. Previous mounts
      // are obscured by the later mounts. Therefore, we must see all entries to
      // make sure we find the last one that matches.
      if (dirAbsPath.get() == hierarchyAbsPath.get()) {
        hierarchyEntry = entry;
      }
    }
  }

  if (hierarchyEntry.isNone()) {
    return Try<std::set<std::string> >::error(
        hierarchy + " is not a mount point for cgroups");
  }

  // Get the intersection of the currently enabled subsystems and mount
  // options. Notice that mount options may contain somethings (e.g. rw) that
  // are not in the set of enabled subsystems.
  Try<std::set<std::string> > names = subsystems();
  if (names.isError()) {
    return Try<std::set<std::string> >::error(names.error());
  }

  std::set<std::string> result;
  foreach (const std::string& name, names.get()) {
    if (hierarchyEntry.get().hasOption(name)) {
      result.insert(name);
    }
  }

  return result;
}


Try<bool> createHierarchy(const std::string& hierarchy,
                          const std::string& subsystems)
{
  if (os::exists(hierarchy)) {
    return Try<bool>::error(
        hierarchy + " already exists in the file system");
  }

  // Make sure all subsystems are enabled.
  Try<bool> enabledResult = enabled(subsystems);
  if (enabledResult.isError()) {
    return Try<bool>::error(enabledResult.error());
  } else if (!enabledResult.get()) {
    return Try<bool>::error("Some subsystems are not enabled");
  }

  // Make sure none of the subsystems are busy.
  Try<bool> busyResult = busy(subsystems);
  if (busyResult.isError()) {
    return Try<bool>::error(busyResult.error());
  } else if (busyResult.get()) {
    return Try<bool>::error("Some subsystems are currently being attached");
  }

  // Create the directory for the hierarchy.
  if (!os::mkdir(hierarchy)) {
    return Try<bool>::error("Failed to create " + hierarchy);
  }

  // Mount the virtual file system (attach subsystems).
  Try<bool> mountResult = internal::mount(hierarchy, subsystems);
  if (mountResult.isError()) {
    os::rmdir(hierarchy);
    return Try<bool>::error(mountResult.error());
  }

  return true;
}


Try<bool> removeHierarchy(const std::string& hierarchy)
{
  Try<bool> check = checkHierarchy(hierarchy);
  if (check.isError()) {
    return Try<bool>::error(check.error());
  }

  Try<bool> unmount = internal::unmount(hierarchy);
  if (unmount.isError()) {
    return Try<bool>::error(unmount.error());
  }

  if (!os::rmdir(hierarchy)) {
    return Try<bool>::error("Failed to remove directory " + hierarchy);
  }

  return true;
}


Try<bool> checkHierarchy(const std::string& hierarchy)
{
  Try<std::set<std::string> > names = subsystems(hierarchy);
  if (names.isError()) {
    return Try<bool>::error(names.error());
  }

  return true;
}


Try<bool> checkHierarchy(const std::string& hierarchy,
                         const std::string& subsystems)
{
  // Check if subsystems are enabled in the system.
  Try<bool> enabledResult = enabled(subsystems);
  if (enabledResult.isError()) {
    return Try<bool>::error(enabledResult.error());
  } else if (!enabledResult.get()) {
    return Try<bool>::error("Some subsystems are not enabled");
  }

  Try<std::set<std::string> > namesResult = cgroups::subsystems(hierarchy);
  if (namesResult.isError()) {
    return Try<bool>::error(namesResult.error());
  }

  std::set<std::string> names = namesResult.get();
  foreach (const std::string& name, strings::split(subsystems, ",")) {
    if (names.find(name) == names.end()) {
      return Try<bool>::error(
          "Subsystem " + name + " is not found or enabled");
    }
  }

  return true;
}


Try<bool> createCgroup(const std::string& hierarchy,
                       const std::string& cgroup)
{
  Try<bool> check = checkHierarchy(hierarchy);
  if (check.isError()) {
    return Try<bool>::error(check.error());
  }

  return internal::createCgroup(hierarchy, cgroup);
}


Try<bool> removeCgroup(const std::string& hierarchy,
                       const std::string& cgroup)
{
  Try<bool> check = checkCgroup(hierarchy, cgroup);
  if (check.isError()) {
    return Try<bool>::error(check.error());
  }

  Try<std::vector<std::string> > cgroups = getCgroups(hierarchy, cgroup);
  if (cgroups.isError()) {
    return Try<bool>::error(cgroups.error());
  }

  if (!cgroups.get().empty()) {
    return Try<bool>::error("Sub-cgroups exist in " + cgroup);
  }

  return internal::removeCgroup(hierarchy, cgroup);
}


Try<bool> checkCgroup(const std::string& hierarchy,
                      const std::string& cgroup)
{
  Try<bool> check = checkHierarchy(hierarchy);
  if (check.isError()) {
    return Try<bool>::error(check.error());
  }

  std::string path = hierarchy + "/" + cgroup;
  if (!os::exists(path)) {
    return Try<bool>::error("Cgroup " + cgroup + " is not valid");
  }

  return true;
}


Try<std::string> readControl(const std::string& hierarchy,
                             const std::string& cgroup,
                             const std::string& control)
{
  Try<bool> check = checkControl(hierarchy, cgroup, control);
  if (check.isError()) {
    return Try<std::string>::error(check.error());
  }

  return internal::readControl(hierarchy, cgroup, control);
}


Try<bool> writeControl(const std::string& hierarchy,
                       const std::string& cgroup,
                       const std::string& control,
                       const std::string& value)
{
  Try<bool> check = checkControl(hierarchy, cgroup, control);
  if (check.isError()) {
    return Try<bool>::error(check.error());
  }

  return internal::writeControl(hierarchy, cgroup, control, value);
}


Try<bool> checkControl(const std::string& hierarchy,
                       const std::string& cgroup,
                       const std::string& control)
{
  Try<bool> check = checkCgroup(hierarchy, cgroup);
  if (check.isError()) {
    return Try<bool>::error(check.error());
  }

  std::string path = hierarchy + "/" + cgroup + "/" + control;
  if (!os::exists(path)) {
    return Try<bool>::error("Control file " + path + " does not exist");
  }

  return true;
}


Try<std::vector<std::string> > getCgroups(const std::string& hierarchy,
                                          const std::string& cgroup)
{
  Try<bool> check = checkCgroup(hierarchy, cgroup);
  if (check.isError()) {
    return Try<std::vector<std::string> >::error(check.error());
  }

  Try<std::string> hierarchyAbsPath = os::realpath(hierarchy);
  if (hierarchyAbsPath.isError()) {
    return Try<std::vector<std::string> >::error(hierarchyAbsPath.error());
  }

  Try<std::string> destAbsPath = os::realpath(hierarchy + "/" + cgroup);
  if (destAbsPath.isError()) {
    return Try<std::vector<std::string> >::error(destAbsPath.error());
  }

  char* paths[] = {const_cast<char*>(destAbsPath.get().c_str()), NULL};

  FTS* tree = fts_open(paths, FTS_NOCHDIR, NULL);
  if (tree == NULL) {
    return Try<std::vector<std::string> >::error(
        "Failed to start file system walk: " +
          std::string(strerror(errno)));
  }

  std::vector<std::string> cgroups;

  FTSENT* node;
  while ((node = fts_read(tree)) != NULL) {
    // Use post-order walk here. fts_level is the depth of the traversal,
    // numbered from -1 to N, where the file/dir was found. The traversal root
    // itself is numbered 0. fts_info includes flags for the current node.
    // FTS_DP indicates a directory being visited in postorder.
    if (node->fts_level > 0 && node->fts_info & FTS_DP) {
      cgroups.push_back(node->fts_path + hierarchyAbsPath.get().length());
    }
  }

  if (errno != 0) {
    return Try<std::vector<std::string> >::error(
        "Failed to read a node during the walk: " +
          std::string(strerror(errno)));
  }

  if (fts_close(tree) != 0) {
    return Try<std::vector<std::string> >::error(
        "Failed to stop a file system walk" +
          std::string(strerror(errno)));
  }

  return cgroups;
}


Try<std::set<pid_t> > getTasks(const std::string& hierarchy,
                               const std::string& cgroup)
{
  Try<bool> check = checkCgroup(hierarchy, cgroup);
  if (check.isError()) {
    return Try<std::set<pid_t> >::error(check.error());
  }

  // Read from the control file.
  Try<std::string> value = internal::readControl(hierarchy, cgroup, "tasks");
  if (value.isError()) {
    return Try<std::set<pid_t> >::error(value.error());
  }

  // Parse the value read from the control file.
  std::set<pid_t> pids;
  std::istringstream ss(value.get());
  ss >> std::dec;
  while (!ss.eof()) {
    pid_t pid;
    ss >> pid;

    if (ss.fail()) {
      if (!ss.eof()) {
        return Try<std::set<pid_t> >::error("Parsing error");
      }
    } else {
      pids.insert(pid);
    }
  }

  return pids;
}


Try<bool> assignTask(const std::string& hierarchy,
                     const std::string& cgroup,
                     pid_t pid)
{
  Try<bool> check = checkCgroup(hierarchy, cgroup);
  if (check.isError()) {
    return Try<bool>::error(check.error());
  }

  return internal::writeControl(hierarchy,
                                cgroup,
                                "tasks",
                                stringify(pid));
}


namespace internal {

#ifndef __NR_eventfd2
#error "The eventfd2 syscall is unavailable."
#endif

#define EFD_SEMAPHORE (1 << 0)
#define EFD_CLOEXEC O_CLOEXEC
#define EFD_NONBLOCK O_NONBLOCK

static int eventfd(unsigned int initval, int flags)
{
  return ::syscall(__NR_eventfd2, initval, flags);
}


// In cgroups, there is mechanism which allows to get notifications about
// changing status of a cgroup. It is based on Linux eventfd. See more
// information in the kernel documentation ("Notification API"). This function
// will create an eventfd and write appropriate control file to correlate the
// eventfd with a type of event so that users can start polling on the eventfd
// to get notified. It returns the eventfd (file descriptor) if the notifier has
// been successfully opened. This function assumes all the parameters are valid.
// The eventfd is set to be non-blocking.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @param   args        Control specific arguments.
// @return  The eventfd if the operation succeeds.
//          Error if the operation fails.
static Try<int> openNotifier(const std::string& hierarchy,
                             const std::string& cgroup,
                             const std::string& control,
                             const Option<std::string>& args =
                               Option<std::string>::none())
{
  int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (efd < 0) {
    return Try<int>::error(
        "Create eventfd failed: " + std::string(strerror(errno)));
  }

  // Open the control file.
  std::string path = hierarchy + "/" + cgroup + "/" + control;
  Try<int> cfd = os::open(path, O_RDWR);
  if (cfd.isError()) {
    os::close(efd);
    return Try<int>::error(cfd.error());
  }

  // Write the event control file (cgroup.event_control).
  std::ostringstream out;
  out << std::dec << efd << " " << cfd.get();
  if (args.isSome()) {
    out << " " << args.get();
  }
  Try<bool> write = internal::writeControl(hierarchy,
                                           cgroup,
                                           "cgroup.event_control",
                                           out.str());
  if (write.isError()) {
    os::close(efd);
    os::close(cfd.get());
    return Try<int>::error(write.error());
  }

  os::close(cfd.get());

  return efd;
}


// Close a notifier. The parameter fd is the eventfd returned by openNotifier.
// @param   fd      The eventfd returned by openNotifier.
// @return  True if the operation succeeds.
//          Error if the operation fails.
static Try<bool> closeNotifier(int fd)
{
  return os::close(fd);
}


// The process listening on event notifier. This class is invisible to users.
class EventListener : public Process<EventListener>
{
public:
  EventListener(const std::string& _hierarchy,
                const std::string& _cgroup,
                const std::string& _control,
                const Option<std::string>& _args)
    : hierarchy(_hierarchy),
      cgroup(_cgroup),
      control(_control),
      args(_args),
      data(0) {}

  virtual ~EventListener() {}

  Future<uint64_t> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop the listener if no one cares. Note that here we explicitly specify
    // the type of the terminate function because it is an overloaded function.
    // The compiler complains if we do not do it.
    promise.future().onDiscarded(lambda::bind(
        static_cast<void (*)(const UPID&, bool)>(terminate), self(), true));

    // Open the event file.
    Try<int> fd = internal::openNotifier(hierarchy, cgroup, control, args);
    if (fd.isError()) {
      promise.fail(fd.error());
      terminate(self());
      return;
    }

    // Remember the opened event file.
    eventfd = fd.get();

    // Perform nonblocking read on the event file. The nonblocking read will
    // start polling on the event file until it becomes readable. If we can
    // successfully read 8 bytes (sizeof uint64_t) from the event file, it
    // indicates an event has occurred.
    reading = io::read(eventfd.get(), &data, sizeof(data));
    reading.onAny(defer(self(), &EventListener::notified));
  }

  virtual void finalize()
  {
    // Discard the nonblocking read.
    reading.discard();

    // Close the eventfd if needed.
    if (eventfd.isSome()) {
      Try<bool> close = internal::closeNotifier(eventfd.get());
      if (close.isError()) {
        LOG(ERROR) << "Closing eventfd " << eventfd.get()
                   << " failed: " << close.error();
      }
    }
  }

private:
  // This function is called when the nonblocking read on the eventfd has
  // result, either because the event has happened, or an error has occurred.
  void notified()
  {
    // Ignore this function if the promise is no longer pending.
    if (!promise.future().isPending()) {
      return;
    }

    // Since the future reading can only be discarded when the promise is no
    // longer pending, we shall never see a discarded reading here because of
    // the check in the beginning of the function.
    CHECK(!reading.isDiscarded());

    if (reading.isFailed()) {
      promise.fail("Failed to read eventfd: " + reading.failure());
    } else {
      if (reading.get() == sizeof(data)) {
        promise.set(data);
      } else {
        promise.fail("Read less than expected");
      }
    }

    terminate(self());
  }

  std::string hierarchy;
  std::string cgroup;
  std::string control;
  Option<std::string> args;
  Promise<uint64_t> promise;
  Future<size_t> reading;
  Option<int> eventfd;  // The eventfd if opened.
  uint64_t data; // The data read from the eventfd.
};

} // namespace internal {


Future<uint64_t> listenEvent(const std::string& hierarchy,
                             const std::string& cgroup,
                             const std::string& control,
                             const Option<std::string>& args)
{
  Try<bool> check = checkControl(hierarchy, cgroup, control);
  if (check.isError()) {
    return Future<uint64_t>::failed(check.error());
  }

  internal::EventListener* listener =
    new internal::EventListener(hierarchy, cgroup, control, args);
  Future<uint64_t> future = listener->future();
  spawn(listener, true);
  return future;
}


namespace internal {


// The process that freezes or thaws the cgroup.
class Freezer : public Process<Freezer>
{
public:
  Freezer(const std::string& _hierarchy,
          const std::string& _cgroup,
          const std::string& _action,
          const seconds& _interval)
    : hierarchy(_hierarchy),
      cgroup(_cgroup),
      action(_action),
      interval(_interval) {}

  virtual ~Freezer() {}

  // Return a future indicating the state of the freezer.
  Future<bool> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop the process if no one cares.
    promise.future().onDiscarded(lambda::bind(
        static_cast<void (*)(const UPID&, bool)>(terminate), self(), true));

    if (interval.value < 0) {
      promise.fail("Invalid interval: " + stringify(interval.value));
      terminate(self());
      return;
    }

    // Start the action.
    if (action == "FREEZE") {
      freeze();
    } else if (action == "THAW") {
      thaw();
    } else {
      promise.fail("Invalid action: " + action);
      terminate(self());
    }
  }

private:
  void freeze()
  {
    Try<bool> result = internal::writeControl(hierarchy,
                                              cgroup,
                                              "freezer.state",
                                              "FROZEN");
    if (result.isError()) {
      promise.fail(result.error());
      terminate(self());
    } else {
      watchFrozen();
    }
  }

  void thaw()
  {
    Try<bool> result = internal::writeControl(hierarchy,
                                              cgroup,
                                              "freezer.state",
                                              "THAWED");
    if (result.isError()) {
      promise.fail(result.error());
      terminate(self());
    } else {
      watchThawed();
    }
  }

  void watchFrozen()
  {
    Try<std::string> state = internal::readControl(hierarchy,
                                                   cgroup,
                                                   "freezer.state");
    if (state.isError()) {
      promise.fail(state.error());
      terminate(self());
      return;
    }

    if (strings::trim(state.get()) == "FROZEN") {
      promise.set(true);
      terminate(self());
    } else if (strings::trim(state.get()) == "FREEZING") {
      // Not done yet, keep watching.
      delay(interval.value, self(), &Freezer::watchFrozen);
    } else {
      LOG(FATAL) << "Unexpected state: " << strings::trim(state.get());
    }
  }

  void watchThawed()
  {
    Try<std::string> state = internal::readControl(hierarchy,
                                                   cgroup,
                                                   "freezer.state");
    if (state.isError()) {
      promise.fail(state.error());
      terminate(self());
      return;
    }

    if (strings::trim(state.get()) == "THAWED") {
      promise.set(true);
      terminate(self());
    } else if (strings::trim(state.get()) == "FROZEN") {
      // Not done yet, keep watching.
      delay(interval.value, self(), &Freezer::watchThawed);
    } else {
      LOG(FATAL) << "Unexpected state: " << strings::trim(state.get());
    }
  }

  std::string hierarchy;
  std::string cgroup;
  std::string action;
  const seconds interval;
  Promise<bool> promise;
};


} // namespace internal {


Future<bool> freezeCgroup(const std::string& hierarchy,
                          const std::string& cgroup,
                          const seconds& interval)
{
  Try<bool> check = checkControl(hierarchy, cgroup, "freezer.state");
  if (check.isError()) {
    return Future<bool>::failed(check.error());
  }

  // Check the current freezer state.
  Try<std::string> state = internal::readControl(hierarchy,
                                                 cgroup,
                                                 "freezer.state");
  if (state.isError()) {
    return Future<bool>::failed(state.error());
  } else if (strings::trim(state.get()) == "FROZEN") {
    return Future<bool>::failed("Cannot freeze a frozen cgroup");
  }

  internal::Freezer* freezer =
    new internal::Freezer(hierarchy, cgroup, "FREEZE", interval);
  Future<bool> future = freezer->future();
  spawn(freezer, true);
  return future;
}


Future<bool> thawCgroup(const std::string& hierarchy,
                        const std::string& cgroup,
                        const seconds& interval)
{
  Try<bool> check = checkControl(hierarchy, cgroup, "freezer.state");
  if (check.isError()) {
    return Future<bool>::failed(check.error());
  }

  // Check the current freezer state.
  Try<std::string> state = internal::readControl(hierarchy,
                                                 cgroup,
                                                 "freezer.state");
  if (state.isError()) {
    return Future<bool>::failed(state.error());
  } else if (strings::trim(state.get()) == "THAWED") {
    return Future<bool>::failed("Cannot thaw a thawed cgroup");
  }

  internal::Freezer* freezer =
    new internal::Freezer(hierarchy, cgroup, "THAW", interval);
  Future<bool> future = freezer->future();
  spawn(freezer, true);
  return future;
}


namespace internal{

// The process used to wait for a cgroup to become empty (no task in it).
class EmptyWatcher: public Process<EmptyWatcher>
{
public:
  EmptyWatcher(const std::string& _hierarchy,
               const std::string& _cgroup,
               const seconds& _interval)
    : hierarchy(_hierarchy),
      cgroup(_cgroup),
      interval(_interval) {}

  virtual ~EmptyWatcher() {}

  // Return a future indicating the state of the watcher.
  Future<bool> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop when no one cares.
    promise.future().onDiscarded(lambda::bind(
        static_cast<void (*)(const UPID&, bool)>(terminate), self(), true));

    if (interval.value < 0) {
      promise.fail("Invalid interval: " + stringify(interval.value));
      terminate(self());
      return;
    }

    check();
  }

private:
  void check()
  {
    Try<std::string> state = internal::readControl(hierarchy,
                                                   cgroup,
                                                   "tasks");
    if (state.isError()) {
      promise.fail(state.error());
      terminate(self());
      return;
    }

    if (strings::trim(state.get()).empty()) {
      promise.set(true);
      terminate(self());
    } else {
      // Re-check needed.
      delay(interval.value, self(), &EmptyWatcher::check);
    }
  }

  std::string hierarchy;
  std::string cgroup;
  const seconds interval;
  Promise<bool> promise;
};


// The process used to atomically kill all tasks in a cgroup.
class TasksKiller : public Process<TasksKiller>
{
public:
  TasksKiller(const std::string& _hierarchy,
              const std::string& _cgroup,
              const seconds& _interval)
    : hierarchy(_hierarchy),
      cgroup(_cgroup),
      interval(_interval) {}

  virtual ~TasksKiller() {}

  // Return a future indicating the state of the killer.
  Future<bool> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop when no one cares.
    promise.future().onDiscarded(lambda::bind(
          static_cast<void (*)(const UPID&, bool)>(terminate), self(), true));

    if (interval.value < 0) {
      promise.fail("Invalid interval: " + stringify(interval.value));
      terminate(self());
      return;
    }

    lambda::function<Future<bool>(const bool&)>
      funcFreeze = defer(self(), &TasksKiller::freeze);
    lambda::function<Future<bool>(const bool&)>
      funcKill = defer(self(), &TasksKiller::kill);
    lambda::function<Future<bool>(const bool&)>
      funcThaw = defer(self(), &TasksKiller::thaw);
    lambda::function<Future<bool>(const bool&)>
      funcEmpty = defer(self(), &TasksKiller::empty);

    Future<bool> finish = Future<bool>(true)
      .then(funcFreeze)   // Freeze the cgroup.
      .then(funcKill)     // Send kill signals to all tasks in the cgroup.
      .then(funcThaw)     // Thaw the cgroup to let kill signals be received.
      .then(funcEmpty);   // Wait until no task in the cgroup.

    finish.onAny(defer(self(), &TasksKiller::finished, finish));
  }

  virtual void finalize()
  {
    // Cancel the operation if the user discards the future.
    if (promise.future().isDiscarded()) {
      // TODO(jieyu): We manually discard the pending futures here because
      // Future::then does not handle cascading discard. The correct semantics
      // for Future::then is: if we discard an "outer" future, we need to make
      // sure we clean up all the other dependent futures as well (cascading
      // discard). This code will be removed once we fix the bug in
      // Future::then function.
      if (futureFreeze.isPending()) {
        futureFreeze.discard();
      } else if (futureThaw.isPending()) {
        futureThaw.discard();
      } else if (futureEmpty.isPending()) {
        futureEmpty.discard();
      }
    }
  }

private:
  Future<bool> freeze()
  {
    futureFreeze = freezeCgroup(hierarchy, cgroup, interval);
    return futureFreeze;
  }

  Future<bool> kill()
  {
    Try<std::set<pid_t> > tasks = getTasks(hierarchy, cgroup);
    if (tasks.isError()) {
      return Future<bool>::failed(tasks.error());
    } else {
      foreach (pid_t pid, tasks.get()) {
        if (::kill(pid, SIGKILL) == -1) {
          return Future<bool>::failed("Failed to kill process " +
            stringify(pid) + ": " + strerror(errno));
        }
      }
    }

    return true;
  }

  Future<bool> thaw()
  {
    futureThaw = thawCgroup(hierarchy, cgroup, interval);
    return futureThaw;
  }

  Future<bool> empty()
  {
    EmptyWatcher* watcher = new EmptyWatcher(hierarchy, cgroup, interval);
    Future<bool> futureEmpty = watcher->future();
    spawn(watcher, true);
    return futureEmpty;
  }

  void finished(const Future<bool>& finish)
  {
    CHECK(!finish.isPending() && !finish.isDiscarded());

    if (finish.isReady()) {
      promise.set(true);
    } else if (finish.isFailed()) {
      promise.fail(finish.failure());
    }

    terminate(self());
  }

  std::string hierarchy;
  std::string cgroup;
  const seconds interval;
  Promise<bool> promise;

  // Intermediate futures (used for asynchronous cancellation).
  Future<bool> futureFreeze;
  Future<bool> futureThaw;
  Future<bool> futureEmpty;
};

} // namespace internal {


Future<bool> killTasks(const std::string& hierarchy,
                       const std::string& cgroup,
                       const seconds& interval)
{
  Try<bool> freezerCheck = checkHierarchy(hierarchy, "freezer");
  if (freezerCheck.isError()) {
    return Future<bool>::failed(freezerCheck.error());
  }

  Try<bool> cgroupCheck = checkCgroup(hierarchy, cgroup);
  if (cgroupCheck.isError()) {
    return Future<bool>::failed(cgroupCheck.error());
  }

  internal::TasksKiller* killer =
    new internal::TasksKiller(hierarchy, cgroup, interval);
  Future<bool> future = killer->future();
  spawn(killer, true);
  return future;
}


namespace internal {

// The process used to destroy a cgroup.
class Destroyer : public Process<Destroyer>
{
public:
  Destroyer(const std::string& _hierarchy,
            const std::vector<std::string>& _cgroups,
            const seconds& _interval)
    : hierarchy(_hierarchy),
      cgroups(_cgroups),
      interval(_interval) {}

  virtual ~Destroyer() {}

  // Return a future indicating the state of the destroyer.
  Future<bool> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop when no one cares.
    promise.future().onDiscarded(lambda::bind(
          static_cast<void (*)(const UPID&, bool)>(terminate), self(), true));

    if (interval.value < 0) {
      promise.fail("Invalid interval: " + stringify(interval.value));
      terminate(self());
      return;
    }

    // Kill tasks in the given cgroups in parallel. Use collect mechanism to
    // wait until all kill processes finish.
    foreach (const std::string& cgroup, cgroups) {
      Future<bool> killer = killTasks(hierarchy, cgroup, interval);
      killers.push_back(killer);
    }

    Future<std::list<bool> > kill = collect(killers);
    kill.onAny(defer(self(), &Destroyer::killed, kill));
  }

  virtual void finalize()
  {
    // Cancel the operation if the user discards the future.
    if (promise.future().isDiscarded()) {
      discard<bool>(killers);
    }
  }

private:
  void killed(const Future<std::list<bool> >& kill)
  {
    if (kill.isReady()) {
      remove();
    } else if (kill.isFailed()) {
      promise.fail(kill.failure());
      terminate(self());
    } else {
      LOG(FATAL) << "Invalid kill state";
    }
  }

  void remove()
  {
    foreach (const std::string& cgroup, cgroups) {
      Try<bool> remove = internal::removeCgroup(hierarchy, cgroup);
      if (remove.isError()) {
        promise.fail(remove.error());
        terminate(self());
        return;
      }
    }

    promise.set(true);
    terminate(self());
  }

  std::string hierarchy;
  std::vector<std::string> cgroups;
  const seconds interval;
  Promise<bool> promise;

  // The killer processes used to atomically kill tasks in each cgroup.
  std::list<Future<bool> > killers;
};

} // namespace internal {


Future<bool> destroyCgroup(const std::string& hierarchy,
                           const std::string& cgroup,
                           const seconds& interval)
{
  Try<bool> cgroupCheck = checkCgroup(hierarchy, cgroup);
  if (cgroupCheck.isError()) {
    return Future<bool>::failed(cgroupCheck.error());
  }

  // Construct the vector of cgroups to destroy.
  Try<std::vector<std::string> > cgroups = getCgroups(hierarchy, cgroup);
  if (cgroups.isError()) {
    return Future<bool>::failed(cgroups.error());
  }

  std::vector<std::string> toDestroy = cgroups.get();
  if (cgroup != "/") {
    toDestroy.push_back(cgroup);
  }

  internal::Destroyer* destroyer =
    new internal::Destroyer(hierarchy, toDestroy, interval);
  Future<bool> future = destroyer->future();
  spawn(destroyer, true);
  return future;
}

} // namespace cgroups {
