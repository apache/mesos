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

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <glog/logging.h>
#include <sys/types.h>

#include <list>
#include <queue>
#include <set>
#include <string>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/version.hpp>

#include <stout/os/access.hpp>
#include <stout/os/bootid.hpp>
#include <stout/os/chdir.hpp>
#include <stout/os/chroot.hpp>
#include <stout/os/dup.hpp>
#include <stout/os/exec.hpp>
#include <stout/os/exists.hpp>
#include <stout/os/fcntl.hpp>
#include <stout/os/getenv.hpp>
#include <stout/os/int_fd.hpp>
#include <stout/os/kill.hpp>
#include <stout/os/ls.hpp>
#include <stout/os/lseek.hpp>
#include <stout/os/lsof.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/mkdtemp.hpp>
#include <stout/os/mktemp.hpp>
#include <stout/os/os.hpp>
#include <stout/os/pagesize.hpp>
#include <stout/os/pipe.hpp>
#include <stout/os/process.hpp>
#include <stout/os/rename.hpp>
#include <stout/os/rm.hpp>
#include <stout/os/rmdir.hpp>
#include <stout/os/shell.hpp>
#include <stout/os/stat.hpp>
#include <stout/os/su.hpp>
#include <stout/os/temp.hpp>
#include <stout/os/touch.hpp>
#include <stout/os/utime.hpp>
#include <stout/os/wait.hpp>
#include <stout/os/xattr.hpp>

#include <stout/os/raw/argv.hpp>
#include <stout/os/raw/environment.hpp>

// For readability, we minimize the number of #ifdef blocks in the code by
// splitting platform specific system calls into separate directories.
#ifdef __WINDOWS__
#include <stout/windows/os.hpp>
#else
#include <stout/posix/os.hpp>
#endif // __WINDOWS__


namespace os {
namespace libraries {
namespace Library {

// Library prefix; e.g., the `lib` in `libprocess`. NOTE: there is no prefix
// on Windows; `libprocess.a` would be `process.lib`.
constexpr const char* prefix =
#ifdef __WINDOWS__
    "";
#else
    "lib";
#endif // __WINDOWS__


// The suffix for a shared library; e.g., `.so` on Linux.
constexpr const char* extension =
#ifdef __APPLE__
    ".dylib";
#elif defined(__WINDOWS__)
    ".dll";
#else
    ".so";
#endif // __APPLE__


// The name of the environment variable that contains paths on which the
// linker should search for libraries. NOTE: Windows does not have an
// environment variable that controls the paths the linker searches through.
constexpr const char* ldPathEnvironmentVariable =
#ifdef __APPLE__
    "DYLD_LIBRARY_PATH";
#elif defined(__WINDOWS__)
    "";
#else
    "LD_LIBRARY_PATH";
#endif

} // namespace Library {

// Returns the full library name by adding prefix and extension to
// library name.
inline std::string expandName(const std::string& libraryName)
{
  return Library::prefix + libraryName + Library::extension;
}


// Returns the current value of LD_LIBRARY_PATH environment variable.
inline std::string paths()
{
  const Option<std::string> path = getenv(Library::ldPathEnvironmentVariable);
  return path.isSome() ? path.get() : std::string();
}


// Updates the value of LD_LIBRARY_PATH environment variable.
// Note that setPaths has an effect only for child processes
// launched after calling it.
inline void setPaths(const std::string& newPaths)
{
  os::setenv(Library::ldPathEnvironmentVariable, newPaths);
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


#ifdef __WINDOWS__
inline Try<std::string> sysname() = delete;
#else
// Return the operating system name (e.g. Linux).
inline Try<std::string> sysname()
{
  Try<UTSInfo> info = uname();
  if (info.isError()) {
    return Error(info.error());
  }

  return info->sysname;
}
#endif // __WINDOWS__


inline Try<std::list<Process>> processes()
{
  const Try<std::set<pid_t>> pids = os::pids();
  if (pids.isError()) {
    return Error(pids.error());
  }

  std::list<Process> result;
  foreach (pid_t pid, pids.get()) {
    const Result<Process> process = os::process(pid);

    // Ignore any processes that disappear between enumeration and now.
    if (process.isSome()) {
      result.push_back(process.get());
    }
  }
  return result;
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


inline Try<std::set<pid_t>> children(pid_t pid, bool recursive = true)
{
  const Try<std::list<Process>> processes = os::processes();

  if (processes.isError()) {
    return Error(processes.error());
  }

  return children(pid, processes.get(), recursive);
}

} // namespace os {

#endif // __STOUT_OS_HPP__
