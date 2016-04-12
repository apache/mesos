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

#ifndef __STOUT_WINDOWS_OS_HPP__
#define __STOUT_WINDOWS_OS_HPP__

#include <direct.h>
#include <io.h>

#include <sys/utime.h>

#include <list>
#include <map>
#include <set>
#include <string>

#include <stout/duration.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>
#include <stout/windows.hpp>

#include <stout/os/os.hpp>
#include <stout/os/read.hpp>

#include <stout/os/raw/environment.hpp>

namespace os {

// Sets the value associated with the specified key in the set of
// environment variables.
inline void setenv(
    const std::string& key,
    const std::string& value,
    bool overwrite = true)
{
  // Do not set the variable if already set and `overwrite` was not specified.
  //
  // Per MSDN[1], `GetEnvironmentVariable` returns 0 on error and sets the
  // error code to `ERROR_ENVVAR_NOT_FOUND` if the variable was not found.
  //
  // [1] https://msdn.microsoft.com/en-us/library/windows/desktop/ms683188(v=vs.85).aspx
  if (!overwrite &&
      ::GetEnvironmentVariable(key.c_str(), NULL, 0) != 0 &&
      ::GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
    return;
  }

  // `SetEnvironmentVariable` returns an error code, but we can't act on it.
  ::SetEnvironmentVariable(key.c_str(), value.c_str());
}


// Unsets the value associated with the specified key in the set of
// environment variables.
inline void unsetenv(const std::string& key) = delete;


// Executes a command by calling "/bin/sh -c <command>", and returns
// after the command has been completed. Returns 0 if succeeds, and
// return -1 on error (e.g., fork/exec/waitpid failed). This function
// is async signal safe. We return int instead of returning a Try
// because Try involves 'new', which is not async signal safe.
inline int system(const std::string& command) = delete;


// This function is a portable version of execvpe ('p' means searching
// executable from PATH and 'e' means setting environments). We add
// this function because it is not available on all systems.
//
// NOTE: This function is not thread safe. It is supposed to be used
// only after fork (when there is only one thread). This function is
// async signal safe.
inline int execvpe(const char* file, char** argv, char** envp) = delete;


inline Try<Nothing> chown(
    uid_t uid,
    gid_t gid,
    const std::string& path,
    bool recursive) = delete;


inline Try<Nothing> chmod(const std::string& path, int mode) = delete;


inline Try<Nothing> mknod(
    const std::string& path,
    mode_t mode,
    dev_t dev) = delete;


// Suspends execution for the given duration.
inline Try<Nothing> sleep(const Duration& duration) = delete;


// Returns the list of files that match the given (shell) pattern.
inline Try<std::list<std::string>> glob(const std::string& pattern) = delete;


// Returns the total number of cpus (cores).
inline Try<long> cpus() = delete;


// Returns load struct with average system loads for the last
// 1, 5 and 15 minutes respectively.
// Load values should be interpreted as usual average loads from
// uptime(1).
inline Try<Load> loadavg() = delete;


// Returns the total size of main and free memory.
inline Try<Memory> memory() = delete;


// Return the system information.
inline Try<UTSInfo> uname() = delete;


inline Try<std::list<Process>> processes() = delete;


// Overload of os::pids for filtering by groups and sessions.
// A group / session id of 0 will fitler on the group / session ID
// of the calling process.
inline Try<std::set<pid_t>> pids(
    Option<pid_t> group,
    Option<pid_t> session) = delete;


inline tm* gmtime_r(const time_t* timep, tm* result)
{
  return ::gmtime_s(result, timep) == ERROR_SUCCESS ? result : NULL;
}

} // namespace os {

#endif // __STOUT_WINDOWS_OS_HPP__
