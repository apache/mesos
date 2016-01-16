// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_OS_HPP__
#define __STOUT_OS_HPP__

#ifdef __WINDOWS__
#include <direct.h>
#endif // __WINDOWS__
#ifdef __sun
#include <sys/loadavg.h>
#define dirfd(dir) ((dir)->d_fd)
#ifndef NAME_MAX
#define NAME_MAX MAXNAMLEN
#endif // NAME_MAX
#else
#include <fts.h>
#endif // __sun
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#ifdef __WINDOWS__
#include <io.h>
#endif // __WINDOWS__
#include <limits.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <utime.h>

#include <glog/logging.h>

#ifdef __linux__
#include <linux/version.h>
#endif // __linux__

#ifdef __linux__
#include <sys/sysinfo.h>
#endif // __linux__
#include <sys/types.h>
#ifdef __WINDOWS__
#include <sys/utime.h>
#endif // __WINDOWS
#include <sys/utsname.h>
#include <sys/wait.h>

#include <list>
#include <queue>
#include <set>
#include <string>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/version.hpp>

#include <stout/os/bootid.hpp>
#include <stout/os/chdir.hpp>
#include <stout/os/environment.hpp>
#include <stout/os/find.hpp>
#include <stout/os/fork.hpp>
#include <stout/os/getcwd.hpp>
#include <stout/os/killtree.hpp>
#include <stout/os/ls.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/mkdtemp.hpp>
#include <stout/os/mktemp.hpp>
#include <stout/os/os.hpp>
#include <stout/os/permissions.hpp>
#include <stout/os/read.hpp>
#include <stout/os/realpath.hpp>
#include <stout/os/rename.hpp>
#include <stout/os/rm.hpp>
#include <stout/os/rmdir.hpp>
#include <stout/os/sendfile.hpp>
#include <stout/os/shell.hpp>
#include <stout/os/signals.hpp>
#include <stout/os/stat.hpp>
#include <stout/os/strerror.hpp>
#include <stout/os/touch.hpp>
#include <stout/os/utime.hpp>
#include <stout/os/write.hpp>


// For readability, we minimize the number of #ifdef blocks in the code by
// splitting platform specifc system calls into separate directories.
#ifdef __WINDOWS__
#include <stout/windows.hpp>
#include <stout/windows/os.hpp>
#else
#include <stout/posix/os.hpp>
#endif // __WINDOWS__


namespace os {


// Looks in the environment variables for the specified key and
// returns a string representation of its value. If no environment
// variable matching key is found, None() is returned.
inline Option<std::string> getenv(const std::string& key)
{
  char* value = ::getenv(key.c_str());

  if (value == NULL) {
    return None();
  }

  return std::string(value);
}


inline Try<bool> access(const std::string& path, int how)
{
  if (::access(path.c_str(), how) < 0) {
    if (errno == EACCES) {
      return false;
    } else {
      return ErrnoError();
    }
  }
  return true;
}


// Creates a tar 'archive' with gzip compression, of the given 'path'.
inline Try<Nothing> tar(const std::string& path, const std::string& archive)
{
  Try<std::string> tarOut =
    os::shell("tar %s %s %s", "-czf", archive.c_str(), path.c_str());

  if (tarOut.isError()) {
    return Error("Failed to archive " + path + ": " + tarOut.error());
  }

  return Nothing();
}


// Return the operating system name (e.g. Linux).
inline Try<std::string> sysname()
{
  Try<UTSInfo> info = uname();
  if (info.isError()) {
    return Error(info.error());
  }

  return info.get().sysname;
}


// Return the OS release numbers.
inline Try<Version> release()
{
  Try<UTSInfo> info = uname();
  if (info.isError()) {
    return Error(info.error());
  }

  int major, minor, patch = 0;
#ifndef __FreeBSD__
  // TODO(karya): Replace sscanf with Version::parse() once Version
  // starts supporting labels and build metadata.
  if (::sscanf(
          info.get().release.c_str(),
          "%d.%d.%d",
          &major,
          &minor,
          &patch) != 3) {
    return Error("Failed to parse: " + info.get().release);
  }
#else
  // TODO(dforsyth): Handle FreeBSD patch versions (-pX).
  if (::sscanf(info.get().release.c_str(), "%d.%d-%*s", &major, &minor) != 2) {
    return Error("Failed to parse: " + info.get().release);
  }
#endif
  return Version(major, minor, patch);
}


inline Option<Process> process(
    pid_t pid,
    const std::list<Process>& processes)
{
  foreach (const Process& process, processes) {
    if (process.pid == pid) {
      return process;
    }
  }
  return None();
}


inline std::set<pid_t> children(
    pid_t pid,
    const std::list<Process>& processes,
    bool recursive = true)
{
  // Perform a breadth first search for descendants.
  std::set<pid_t> descendants;
  std::queue<pid_t> parents;
  parents.push(pid);

  do {
    pid_t parent = parents.front();
    parents.pop();

    // Search for children of parent.
    foreach (const Process& process, processes) {
      if (process.parent == parent) {
        // Have we seen this child yet?
        if (descendants.insert(process.pid).second) {
          parents.push(process.pid);
        }
      }
    }
  } while (recursive && !parents.empty());

  return descendants;
}


inline Try<std::set<pid_t> > children(pid_t pid, bool recursive = true)
{
  const Try<std::list<Process>> processes = os::processes();

  if (processes.isError()) {
    return Error(processes.error());
  }

  return children(pid, processes.get(), recursive);
}


namespace libraries {

// Returns the full library name by adding prefix and extension to
// library name.
inline std::string expandName(const std::string& libraryName)
{
  const char* prefix = "lib";
  const char* extension =
#ifdef __APPLE__
    ".dylib";
#else
    ".so";
#endif

  return prefix + libraryName + extension;
}


// Returns the current value of LD_LIBRARY_PATH environment variable.
inline std::string paths()
{
  const char* environmentVariable =
#ifdef __APPLE__
    "DYLD_LIBRARY_PATH";
#else
    "LD_LIBRARY_PATH";
#endif
  const Option<std::string> path = getenv(environmentVariable);
  return path.isSome() ? path.get() : std::string();
}


// Updates the value of LD_LIBRARY_PATH environment variable.
inline void setPaths(const std::string& newPaths)
{
  const char* environmentVariable =
#ifdef __APPLE__
    "DYLD_LIBRARY_PATH";
#else
    "LD_LIBRARY_PATH";
#endif
  setenv(environmentVariable, newPaths);
}


// Append newPath to the current value of LD_LIBRARY_PATH environment
// variable.
inline void appendPaths(const std::string& newPaths)
{
  if (paths().empty()) {
    setPaths(newPaths);
  } else {
    setPaths(paths() + ":" + newPaths);
  }
}

} // namespace libraries {
} // namespace os {

#endif // __STOUT_OS_HPP__
