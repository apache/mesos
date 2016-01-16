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

#ifndef __STOUT_POSIX_OS_HPP__
#define __STOUT_POSIX_OS_HPP__

#include <errno.h>
#ifdef __sun
#include <sys/loadavg.h>
#else
#include <fts.h>
#endif // __sun
#include <glob.h>
#include <grp.h>
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
#include <sys/utsname.h>
#include <sys/wait.h>

#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/close.hpp>
#include <stout/os/exists.hpp>
#include <stout/os/fcntl.hpp>
#include <stout/os/fork.hpp>
#ifdef __FreeBSD__
#include <stout/os/freebsd.hpp>
#endif
#ifdef __linux__
#include <stout/os/linux.hpp>
#endif // __linux__
#include <stout/os/open.hpp>
#ifdef __APPLE__
#include <stout/os/osx.hpp>
#endif // __APPLE__
#ifdef __sun
#include <stout/os/sunos.hpp>
#endif // __sun
#ifdef __APPLE__
#include <stout/os/sysctl.hpp>
#endif // __APPLE__

#include <stout/os/raw/environment.hpp>

namespace os {

// Forward declarations.
inline Try<Nothing> utime(const std::string&);

// Sets the value associated with the specified key in the set of
// environment variables.
inline void setenv(const std::string& key,
                   const std::string& value,
                   bool overwrite = true)
{
  ::setenv(key.c_str(), value.c_str(), overwrite ? 1 : 0);
}


// Unsets the value associated with the specified key in the set of
// environment variables.
inline void unsetenv(const std::string& key)
{
  ::unsetenv(key.c_str());
}


// Executes a command by calling "/bin/sh -c <command>", and returns
// after the command has been completed. Returns 0 if succeeds, and
// return -1 on error (e.g., fork/exec/waitpid failed). This function
// is async signal safe. We return int instead of returning a Try
// because Try involves 'new', which is not async signal safe.
inline int system(const std::string& command)
{
  pid_t pid = ::fork();

  if (pid == -1) {
    return -1;
  } else if (pid == 0) {
    // In child process.
    ::execlp("sh", "sh", "-c", command.c_str(), (char*) NULL);
    ::exit(127);
  } else {
    // In parent process.
    int status;
    while (::waitpid(pid, &status, 0) == -1) {
      if (errno != EINTR) {
        return -1;
      }
    }

    return status;
  }
}


// This function is a portable version of execvpe ('p' means searching
// executable from PATH and 'e' means setting environments). We add
// this function because it is not available on all systems.
//
// NOTE: This function is not thread safe. It is supposed to be used
// only after fork (when there is only one thread). This function is
// async signal safe.
inline int execvpe(const char* file, char** argv, char** envp)
{
  char** saved = os::raw::environment();

  *os::raw::environmentp() = envp;

  int result = execvp(file, argv);

  *os::raw::environmentp() = saved;

  return result;
}


inline Try<Nothing> chown(
    uid_t uid,
    gid_t gid,
    const std::string& path,
    bool recursive)
{
  if (recursive) {
    // TODO(bmahler): Consider walking the file tree instead. We would need
    // to be careful to not miss dotfiles.
    std::string command =
      "chown -R " + stringify(uid) + ':' + stringify(gid) + " '" + path + "'";

    int status = os::system(command);
    if (status != 0) {
      return ErrnoError(
          "Failed to execute '" + command +
          "' (exit status: " + stringify(status) + ")");
    }
  } else {
    if (::chown(path.c_str(), uid, gid) < 0) {
      return ErrnoError();
    }
  }

  return Nothing();
}


// Changes the specified path's user and group ownership to that of
// the specified user.
inline Try<Nothing> chown(
    const std::string& user,
    const std::string& path,
    bool recursive = true)
{
  passwd* passwd;
  if ((passwd = ::getpwnam(user.c_str())) == NULL) {
    return ErrnoError("Failed to get user information for '" + user + "'");
  }

  return chown(passwd->pw_uid, passwd->pw_gid, path, recursive);
}


inline Try<Nothing> chmod(const std::string& path, int mode)
{
  if (::chmod(path.c_str(), mode) < 0) {
    return ErrnoError();
  }

  return Nothing();
}


inline Try<Nothing> chroot(const std::string& directory)
{
  if (::chroot(directory.c_str()) < 0) {
    return ErrnoError();
  }

  return Nothing();
}


inline Try<Nothing> mknod(
    const std::string& path,
    mode_t mode,
    dev_t dev)
{
  if (::mknod(path.c_str(), mode, dev) < 0) {
    return ErrnoError();
  }

  return Nothing();
}


inline Result<uid_t> getuid(const Option<std::string>& user = None())
{
  if (user.isNone()) {
    return ::getuid();
  }

  struct passwd passwd;
  struct passwd* result = NULL;

  int size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (size == -1) {
    // Initial value for buffer size.
    size = 1024;
  }

  while (true) {
    char* buffer = new char[size];

    if (getpwnam_r(user.get().c_str(), &passwd, buffer, size, &result) == 0) {
      // The usual interpretation of POSIX is that getpwnam_r will
      // return 0 but set result == NULL if the user is not found.
      if (result == NULL) {
        delete[] buffer;
        return None();
      }

      uid_t uid = passwd.pw_uid;
      delete[] buffer;
      return uid;
    } else {
      // RHEL7 (and possibly other systems) will return non-zero and
      // set one of the following errors for "The given name or uid
      // was not found." See 'man getpwnam_r'. We only check for the
      // errors explicitly listed, and do not consider the ellipsis.
      if (errno == ENOENT ||
          errno == ESRCH ||
          errno == EBADF ||
          errno == EPERM) {
        delete[] buffer;
        return None();
      }

      if (errno != ERANGE) {
        delete[] buffer;
        return ErrnoError("Failed to get username information");
      }
      // getpwnam_r set ERANGE so try again with a larger buffer.
      size *= 2;
      delete[] buffer;
    }
  }

  UNREACHABLE();
}


inline Result<gid_t> getgid(const Option<std::string>& user = None())
{
  if (user.isNone()) {
    return ::getgid();
  }

  struct passwd passwd;
  struct passwd* result = NULL;

  int size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (size == -1) {
    // Initial value for buffer size.
    size = 1024;
  }

  while (true) {
    char* buffer = new char[size];

    if (getpwnam_r(user.get().c_str(), &passwd, buffer, size, &result) == 0) {
      // The usual interpretation of POSIX is that getpwnam_r will
      // return 0 but set result == NULL if the group is not found.
      if (result == NULL) {
        delete[] buffer;
        return None();
      }

      gid_t gid = passwd.pw_gid;
      delete[] buffer;
      return gid;
    } else {
      // RHEL7 (and possibly other systems) will return non-zero and
      // set one of the following errors for "The given name or uid
      // was not found." See 'man getpwnam_r'. We only check for the
      // errors explicitly listed, and do not consider the ellipsis.
      if (errno == ENOENT ||
          errno == ESRCH ||
          errno == EBADF ||
          errno == EPERM) {
        delete[] buffer;
        return None();
      }

      if (errno != ERANGE) {
        delete[] buffer;
        return ErrnoError("Failed to get username information");
      }
      // getpwnam_r set ERANGE so try again with a larger buffer.
      size *= 2;
      delete[] buffer;
    }
  }

  UNREACHABLE();
}


inline Try<Nothing> su(const std::string& user)
{
  Result<gid_t> gid = os::getgid(user);
  if (gid.isError() || gid.isNone()) {
    return Error("Failed to getgid: " +
        (gid.isError() ? gid.error() : "unknown user"));
  } else if (::setgid(gid.get())) {
    return ErrnoError("Failed to set gid");
  }

  // Set the supplementary group list. We ignore EPERM because
  // performing a no-op call (switching to same group) still
  // requires being privileged, unlike 'setgid' and 'setuid'.
  if (::initgroups(user.c_str(), gid.get()) == -1 && errno != EPERM) {
    return ErrnoError("Failed to set supplementary groups");
  }

  Result<uid_t> uid = os::getuid(user);
  if (uid.isError() || uid.isNone()) {
    return Error("Failed to getuid: " +
        (uid.isError() ? uid.error() : "unknown user"));
  } else if (::setuid(uid.get())) {
    return ErrnoError("Failed to setuid");
  }

  return Nothing();
}


inline Result<std::string> user(Option<uid_t> uid = None())
{
  if (uid.isNone()) {
    uid = ::getuid();
  }

  int size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (size == -1) {
    // Initial value for buffer size.
    size = 1024;
  }

  struct passwd passwd;
  struct passwd* result = NULL;

  while (true) {
    char* buffer = new char[size];

    if (getpwuid_r(uid.get(), &passwd, buffer, size, &result) == 0) {
      // getpwuid_r will return 0 but set result == NULL if the uid is
      // not found.
      if (result == NULL) {
        delete[] buffer;
        return None();
      }

      std::string user(passwd.pw_name);
      delete[] buffer;
      return user;
    } else {
      if (errno != ERANGE) {
        delete[] buffer;
        return ErrnoError();
      }

      // getpwuid_r set ERANGE so try again with a larger buffer.
      size *= 2;
      delete[] buffer;
    }
  }
}


// Suspends execution for the given duration.
inline Try<Nothing> sleep(const Duration& duration)
{
  timespec remaining;
  remaining.tv_sec = static_cast<long>(duration.secs());
  remaining.tv_nsec =
    static_cast<long>((duration - Seconds(remaining.tv_sec)).ns());

  while (nanosleep(&remaining, &remaining) == -1) {
    if (errno == EINTR) {
      continue;
    } else {
      return ErrnoError();
    }
  }

  return Nothing();
}


// Returns the list of files that match the given (shell) pattern.
inline Try<std::list<std::string>> glob(const std::string& pattern)
{
  glob_t g;
  int status = ::glob(pattern.c_str(), GLOB_NOSORT, NULL, &g);

  std::list<std::string> result;

  if (status != 0) {
    if (status == GLOB_NOMATCH) {
      return result; // Empty list.
    } else {
      return ErrnoError();
    }
  }

  for (size_t i = 0; i < g.gl_pathc; ++i) {
    result.push_back(g.gl_pathv[i]);
  }

  globfree(&g); // Best-effort free of dynamically allocated memory.

  return result;
}


// Returns the total number of cpus (cores).
inline Try<long> cpus()
{
  long cpus = sysconf(_SC_NPROCESSORS_ONLN);

  if (cpus < 0) {
    return ErrnoError();
  }
  return cpus;
}


// Returns load struct with average system loads for the last
// 1, 5 and 15 minutes respectively.
// Load values should be interpreted as usual average loads from
// uptime(1).
inline Try<Load> loadavg()
{
  double loadArray[3];
  if (getloadavg(loadArray, 3) == -1) {
    return ErrnoError("Failed to determine system load averages");
  }

  Load load;
  load.one = loadArray[0];
  load.five = loadArray[1];
  load.fifteen = loadArray[2];

  return load;
}


// Returns the total size of main and free memory.
inline Try<Memory> memory()
{
  Memory memory;

#ifdef __linux__
  struct sysinfo info;
  if (sysinfo(&info) != 0) {
    return ErrnoError();
  }

# if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 3, 23)
  memory.total = Bytes(info.totalram * info.mem_unit);
  memory.free = Bytes(info.freeram * info.mem_unit);
  memory.totalSwap = Bytes(info.totalswap * info.mem_unit);
  memory.freeSwap = Bytes(info.freeswap * info.mem_unit);
# else
  memory.total = Bytes(info.totalram);
  memory.free = Bytes(info.freeram);
  memory.totalSwap = Bytes(info.totalswap);
  memory.freeSwap = Bytes(info.freeswap);
# endif

  return memory;

#elif defined __APPLE__
  const Try<int64_t> totalMemory = os::sysctl(CTL_HW, HW_MEMSIZE).integer();

  if (totalMemory.isError()) {
    return Error(totalMemory.error());
  }
  memory.total = Bytes(totalMemory.get());

  // Size of free memory is available in terms of number of
  // free pages on Mac OS X.
  const long pageSize = sysconf(_SC_PAGESIZE);
  if (pageSize < 0) {
    return ErrnoError();
  }

  unsigned int freeCount;
  size_t length = sizeof(freeCount);

  if (sysctlbyname(
      "vm.page_free_count",
      &freeCount,
      &length,
      NULL,
      0) != 0) {
    return ErrnoError();
  }
  memory.free = Bytes(freeCount * pageSize);

  struct xsw_usage usage;
  length = sizeof(struct xsw_usage);
  if (sysctlbyname(
        "vm.swapusage",
        &usage,
        &length,
        NULL,
        0) != 0) {
    return ErrnoError();
  }
  memory.totalSwap = Bytes(usage.xsu_total * pageSize);
  memory.freeSwap = Bytes(usage.xsu_avail * pageSize);

  return memory;

#else
  return Error("Cannot determine the size of total and free memory");
#endif
}


// Return the system information.
inline Try<UTSInfo> uname()
{
  struct utsname name;

  if (::uname(&name) < 0) {
    return ErrnoError();
  }

  UTSInfo info;
  info.sysname = name.sysname;
  info.nodename = name.nodename;
  info.release = name.release;
  info.version = name.version;
  info.machine = name.machine;
  return info;
}


inline Try<std::list<Process>> processes()
{
  const Try<std::set<pid_t>> pids = os::pids();

  if (pids.isError()) {
    return Error(pids.error());
  }

  std::list<Process> result;
  foreach (pid_t pid, pids.get()) {
    const Result<Process> process = os::process(pid);

    // Ignore any processes that disappear.
    if (process.isSome()) {
      result.push_back(process.get());
    }
  }
  return result;
}


// Overload of os::pids for filtering by groups and sessions.
// A group / session id of 0 will fitler on the group / session ID
// of the calling process.
inline Try<std::set<pid_t>> pids(Option<pid_t> group, Option<pid_t> session)
{
  if (group.isNone() && session.isNone()) {
    return os::pids();
  } else if (group.isSome() && group.get() < 0) {
    return Error("Invalid group");
  } else if (session.isSome() && session.get() < 0) {
    return Error("Invalid session");
  }

  const Try<std::list<Process>> processes = os::processes();

  if (processes.isError()) {
    return Error(processes.error());
  }

  // Obtain the calling process group / session ID when 0 is provided.
  if (group.isSome() && group.get() == 0) {
    group = getpgid(0);
  }
  if (session.isSome() && session.get() == 0) {
    session = getsid(0);
  }

  std::set<pid_t> result;
  foreach (const Process& process, processes.get()) {
    // Group AND Session (intersection).
    if (group.isSome() && session.isSome()) {
      if (group.get() == process.group &&
          process.session.isSome() &&
          session.get() == process.session.get()) {
        result.insert(process.pid);
      }
    } else if (group.isSome() && group.get() == process.group) {
      result.insert(process.pid);
    } else if (session.isSome() && process.session.isSome() &&
               session.get() == process.session.get()) {
      result.insert(process.pid);
    }
  }

  return result;
}

} // namespace os {

#endif // __STOUT_POSIX_OS_HPP__
