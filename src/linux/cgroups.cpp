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
#include <unistd.h>

#include <fstream>
#include <map>
#include <sstream>

#include "common/foreach.hpp"
#include "common/strings.hpp"
#include "common/utils.hpp"

#include "linux/cgroups.hpp"
#include "linux/fs.hpp"


using namespace mesos::internal;


namespace cgroups {


//////////////////////////////////////////////////////////////////////////////
// The following *internal* functions provide very basic controls over Linux
// cgroups. Sanity checks are removed from these functions for performance.
// Users can always wrap these functions to provide various checks if needed.
//////////////////////////////////////////////////////////////////////////////


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

  // We do not use utils::os::read here because it cannot correctly read proc or
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


//////////////////////////////////////////////////////////////////////////////
// The following functions are visible to users.
//////////////////////////////////////////////////////////////////////////////


bool enabled()
{
  return utils::os::exists("/proc/cgroups");
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
  Try<std::string> hierarchyAbsPath = utils::os::realpath(hierarchy);
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
      Try<std::string> dirAbsPath = utils::os::realpath(entry.dir);
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
  if (utils::os::exists(hierarchy)) {
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
  if (!utils::os::mkdir(hierarchy)) {
    return Try<bool>::error("Failed to create " + hierarchy);
  }

  // Mount the virtual file system (attach subsystems).
  Try<bool> mountResult = internal::mount(hierarchy, subsystems);
  if (mountResult.isError()) {
    utils::os::rmdir(hierarchy);
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

  if (!utils::os::rmdir(hierarchy)) {
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
  if (!utils::os::exists(path)) {
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
  if (!utils::os::exists(path)) {
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

  Try<std::string> hierarchyAbsPath = utils::os::realpath(hierarchy);
  if (hierarchyAbsPath.isError()) {
    return Try<std::vector<std::string> >::error(hierarchyAbsPath.error());
  }

  Try<std::string> destAbsPath = utils::os::realpath(hierarchy + "/" + cgroup);
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
                                utils::stringify(pid));
}


} // namespace cgroups {
