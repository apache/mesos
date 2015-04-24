/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_OS_HPP__
#define __STOUT_OS_HPP__

#ifdef __APPLE__
#include <crt_externs.h> // For _NSGetEnviron().
#endif
#include <dirent.h>
#include <errno.h>
#include <fts.h>
#include <glob.h>
#include <grp.h>
#include <libgen.h>
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
#include <set>
#include <string>

#include <stout/bytes.hpp>
#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>
#include <stout/version.hpp>

#include <stout/os/close.hpp>
#include <stout/os/exists.hpp>
#include <stout/os/fcntl.hpp>
#include <stout/os/fork.hpp>
#include <stout/os/killtree.hpp>
#ifdef __linux__
#include <stout/os/linux.hpp>
#endif // __linux__
#include <stout/os/ls.hpp>
#include <stout/os/open.hpp>
#ifdef __APPLE__
#include <stout/os/osx.hpp>
#endif // __APPLE__
#include <stout/os/permissions.hpp>
#include <stout/os/pstree.hpp>
#include <stout/os/read.hpp>
#include <stout/os/rename.hpp>
#include <stout/os/sendfile.hpp>
#include <stout/os/shell.hpp>
#include <stout/os/signals.hpp>
#include <stout/os/stat.hpp>
#ifdef __APPLE__
#include <stout/os/sysctl.hpp>
#endif // __APPLE__

#ifdef __APPLE__
// Assigning the result pointer to ret silences an unused var warning.
#define gethostbyname2_r(name, af, ret, buf, buflen, result, h_errnop)  \
  ({ (void)ret; *(result) = gethostbyname2(name, af); 0; })
#endif // __APPLE__

// Need to declare 'environ' pointer for non OS X platforms.
#ifndef __APPLE__
extern char** environ;
#endif

namespace os {

inline char** environ()
{
  // Accessing the list of environment variables is platform-specific.
  // On OS X, the 'environ' symbol isn't visible to shared libraries,
  // so we must use the _NSGetEnviron() function (see 'man environ' on
  // OS X). On other platforms, it's fine to access 'environ' from
  // shared libraries.
#ifdef __APPLE__
  return *_NSGetEnviron();
#else
  return ::environ;
#endif
}


// Returns the address of os::environ().
inline char*** environp()
{
  // Accessing the list of environment variables is platform-specific.
  // On OS X, the 'environ' symbol isn't visible to shared libraries,
  // so we must use the _NSGetEnviron() function (see 'man environ' on
  // OS X). On other platforms, it's fine to access 'environ' from
  // shared libraries.
#ifdef __APPLE__
  return _NSGetEnviron();
#else
  return &::environ;
#endif
}


inline hashmap<std::string, std::string> environment()
{
  char** environ = os::environ();

  hashmap<std::string, std::string> result;

  for (size_t index = 0; environ[index] != NULL; index++) {
    std::string entry(environ[index]);
    size_t position = entry.find_first_of('=');
    if (position == std::string::npos) {
      continue; // Skip malformed environment entries.
    }
    result[entry.substr(0, position)] = entry.substr(position + 1);
  }

  return result;
}


// Checks if the specified key is in the environment variables.
inline bool hasenv(const std::string& key)
{
  char* value = ::getenv(key.c_str());

  return value != NULL;
}

// Looks in the environment variables for the specified key and
// returns a string representation of it's value. If 'expected' is
// true (default) and no environment variable matching key is found,
// this function will exit the process.
inline std::string getenv(const std::string& key, bool expected = true)
{
  char* value = ::getenv(key.c_str());

  if (expected && value == NULL) {
    LOG(FATAL) << "Expecting '" << key << "' in environment variables";
  }

  if (value != NULL) {
    return std::string(value);
  }

  return std::string();
}


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


// Sets the access and modification times of 'path' to the current time.
inline Try<Nothing> utime(const std::string& path)
{
  if (::utime(path.c_str(), NULL) == -1) {
    return ErrnoError();
  }

  return Nothing();
}


inline Try<Nothing> touch(const std::string& path)
{
  if (!exists(path)) {
    Try<int> fd = open(
        path,
        O_RDWR | O_CREAT,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if (fd.isError()) {
      return Error("Failed to open file: " + fd.error());
    }

    return close(fd.get());
  }

  // Update the access and modification times.
  return utime(path);
}


// Creates a temporary file using the specified path template. The
// template may be any path with _6_ `Xs' appended to it, for example
// /tmp/temp.XXXXXX. The trailing `Xs' are replaced with a unique
// alphanumeric combination.
inline Try<std::string> mktemp(const std::string& path = "/tmp/XXXXXX")
{
  char* temp = new char[path.size() + 1];
  int fd = ::mkstemp(::strcpy(temp, path.c_str()));

  if (fd < 0) {
    delete[] temp;
    return ErrnoError();
  }

  // We ignore the return value of close(). This is because users
  // calling this function are interested in the return value of
  // mkstemp(). Also an unsuccessful close() doesn't affect the file.
  os::close(fd);

  std::string result(temp);
  delete[] temp;
  return result;
}


// Write out the string to the file at the current fd position.
inline Try<Nothing> write(int fd, const std::string& message)
{
  size_t offset = 0;

  while (offset < message.length()) {
    ssize_t length =
      ::write(fd, message.data() + offset, message.length() - offset);

    if (length < 0) {
      // TODO(benh): Handle a non-blocking fd? (EAGAIN, EWOULDBLOCK)
      if (errno == EINTR) {
        continue;
      }
      return ErrnoError();
    }

    offset += length;
  }

  return Nothing();
}


// A wrapper function that wraps the above write() with
// open and closing the file.
inline Try<Nothing> write(const std::string& path, const std::string& message)
{
  Try<int> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (fd.isError()) {
    return ErrnoError("Failed to open file '" + path + "'");
  }

  Try<Nothing> result = write(fd.get(), message);

  // We ignore the return value of close(). This is because users
  // calling this function are interested in the return value of
  // write(). Also an unsuccessful close() doesn't affect the write.
  os::close(fd.get());

  return result;
}


inline Try<Nothing> rm(const std::string& path)
{
  if (::remove(path.c_str()) != 0) {
    return ErrnoError();
  }

  return Nothing();
}


inline Try<std::string> basename(const std::string& path)
{
  char* temp = new char[path.size() + 1];
  char* result = ::basename(::strcpy(temp, path.c_str()));
  if (result == NULL) {
    delete[] temp;
    return ErrnoError();
  }

  std::string s(result);
  delete[] temp;
  return s;
}


inline Try<std::string> dirname(const std::string& path)
{
  char* temp = new char[path.size() + 1];
  char* result = ::dirname(::strcpy(temp, path.c_str()));
  if (result == NULL) {
    delete[] temp;
    return ErrnoError();
  }

  std::string s(result);
  delete[] temp;
  return s;
}


inline Result<std::string> realpath(const std::string& path)
{
  char temp[PATH_MAX];
  if (::realpath(path.c_str(), temp) == NULL) {
    if (errno == ENOENT || errno == ENOTDIR) {
      return None();
    }
    return ErrnoError();
  }
  return std::string(temp);
}


inline Try<Nothing> mkdir(const std::string& directory, bool recursive = true)
{
  if (!recursive) {
    if (::mkdir(directory.c_str(), 0755) < 0) {
      return ErrnoError();
    }
  } else {
    std::vector<std::string> tokens = strings::tokenize(directory, "/");
    std::string path = "";

    // We got an absolute path, so keep the leading slash.
    if (directory.find_first_of("/") == 0) {
      path = "/";
    }

    foreach (const std::string& token, tokens) {
      path += token;
      if (::mkdir(path.c_str(), 0755) < 0 && errno != EEXIST) {
        return ErrnoError();
      }
      path += "/";
    }
  }

  return Nothing();
}

// Creates a temporary directory using the specified path
// template. The template may be any path with _6_ `Xs' appended to
// it, for example /tmp/temp.XXXXXX. The trailing `Xs' are replaced
// with a unique alphanumeric combination.
inline Try<std::string> mkdtemp(const std::string& path = "/tmp/XXXXXX")
{
  char* temp = new char[path.size() + 1];
  if (::mkdtemp(::strcpy(temp, path.c_str())) != NULL) {
    std::string result(temp);
    delete[] temp;
    return result;
  } else {
    delete[] temp;
    return ErrnoError();
  }
}

// By default, recursively deletes a directory akin to: 'rm -r'. If the
// programmer sets recursive to false, it deletes a directory akin to: 'rmdir'.
// Note that this function expects an absolute path.
inline Try<Nothing> rmdir(const std::string& directory, bool recursive = true)
{
  if (!recursive) {
    if (::rmdir(directory.c_str()) < 0) {
      return ErrnoError();
    }
  } else {
    char* paths[] = {const_cast<char*>(directory.c_str()), NULL};

    FTS* tree = fts_open(paths, FTS_NOCHDIR, NULL);
    if (tree == NULL) {
      return ErrnoError();
    }

    FTSENT* node;
    while ((node = fts_read(tree)) != NULL) {
      switch (node->fts_info) {
        case FTS_DP:
          if (::rmdir(node->fts_path) < 0 && errno != ENOENT) {
            return ErrnoError();
          }
          break;
        case FTS_F:
        case FTS_SL:
          if (::unlink(node->fts_path) < 0 && errno != ENOENT) {
            return ErrnoError();
          }
          break;
        default:
          break;
      }
    }

    if (errno != 0) {
      return ErrnoError();
    }

    if (fts_close(tree) < 0) {
      return ErrnoError();
    }
  }

  return Nothing();
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
    ::execl("/bin/sh", "sh", "-c", command.c_str(), (char*) NULL);
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
  char** saved = os::environ();

  *os::environp() = envp;

  int result = execvp(file, argv);

  *os::environp() = saved;

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


inline Try<Nothing> chdir(const std::string& directory)
{
  if (::chdir(directory.c_str()) < 0) {
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


inline std::string getcwd()
{
  size_t size = 100;

  while (true) {
    char* temp = new char[size];
    if (::getcwd(temp, size) == temp) {
      std::string result(temp);
      delete[] temp;
      return result;
    } else {
      if (errno != ERANGE) {
        delete[] temp;
        return std::string();
      }
      size *= 2;
      delete[] temp;
    }
  }

  return std::string();
}


// Return the list of file paths that match the given pattern by recursively
// searching the given directory. A match is successful if the pattern is a
// substring of the file name.
// NOTE: Directory path should not end with '/'.
// NOTE: Symbolic links are not followed.
// TODO(vinod): Support regular expressions for pattern.
// TODO(vinod): Consider using ftw or a non-recursive approach.
inline Try<std::list<std::string> > find(
    const std::string& directory,
    const std::string& pattern)
{
  std::list<std::string> results;

  if (!stat::isdir(directory)) {
    return Error("'" + directory + "' is not a directory");
  }

  Try<std::list<std::string> > entries = ls(directory);
  if (entries.isSome()) {
    foreach (const std::string& entry, entries.get()) {
      std::string path = path::join(directory, entry);
      // If it's a directory, recurse.
      if (stat::isdir(path) && !stat::islink(path)) {
        Try<std::list<std::string> > matches = find(path, pattern);
        if (matches.isError()) {
          return matches;
        }
        foreach (const std::string& match, matches.get()) {
          results.push_back(match);
        }
      } else {
        if (entry.find(pattern) != std::string::npos) {
          results.push_back(path); // Matched the file pattern!
        }
      }
    }
  }

  return results;
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


// Creates a tar 'archive' with gzip compression, of the given 'path'.
inline Try<Nothing> tar(const std::string& path, const std::string& archive)
{
  Try<int> status =
    shell(NULL, "tar -czf %s %s", archive.c_str(), path.c_str());

  if (status.isError()) {
    return Error("Failed to archive " + path + ": " + status.error());
  } else if (status.get() != 0) {
    return Error("Non-zero exit status when archiving " + path +
                 ": " + stringify(status.get()));
  }

  return Nothing();
}


// Returns the list of files that match the given (shell) pattern.
inline Try<std::list<std::string> > glob(const std::string& pattern)
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


// Structure returned by loadavg(). Encodes system load average
// for the last 1, 5 and 15 minutes.
struct Load {
  double one;
  double five;
  double fifteen;
};


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


// Structure returned by memory() containing the total size of main
// and free memory.
struct Memory
{
  Bytes total;
  Bytes free;
};


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
# else
  memory.total = Bytes(info.totalram);
  memory.free = Bytes(info.freeram);
# endif

  return memory;

#elif defined __APPLE__
  const Try<int64_t>& totalMemory =
    os::sysctl(CTL_HW, HW_MEMSIZE).integer();

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

  return memory;

#else
  return Error("Cannot determine the size of total and free memory");
#endif
}


inline Try<std::string> bootId()
{
#ifdef __linux__
  Try<std::string> read = os::read("/proc/sys/kernel/random/boot_id");
  if (read.isError()) {
    return read;
  }
  return strings::trim(read.get());
#elif defined(__APPLE__)
  // For OS X, we use the boot time in seconds as a unique boot id.
  // Although imperfect, this works quite well in practice.
  Try<timeval> bootTime = os::sysctl(CTL_KERN, KERN_BOOTTIME).time();
  if (bootTime.isError()) {
    return Error(bootTime.error());
  }
  return stringify(bootTime.get().tv_sec);
#else
  return Error("Not implemented");
#endif
}


// The structure returned by uname describing the currently running system.
struct UTSInfo
{
  std::string sysname;    // Operating system name (e.g. Linux).
  std::string nodename;   // Network name of this machine.
  std::string release;    // Release level of the operating system.
  std::string version;    // Version level of the operating system.
  std::string machine;    // Machine hardware platform.
};


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

  // TODO(karya): Replace sscanf with Version::parse() once Version
  // starts supporting labels and build metadata.
  int major, minor, patch;
  if (::sscanf(
          info.get().release.c_str(),
          "%d.%d.%d",
          &major,
          &minor,
          &patch) != 3) {
    return Error("Failed to parse: " + info.get().release);
  }

  return Version(major, minor, patch);
}


inline Try<std::list<Process> > processes()
{
  const Try<std::set<pid_t> >& pids = os::pids();

  if (pids.isError()) {
    return Error(pids.error());
  }

  std::list<Process> result;
  foreach (pid_t pid, pids.get()) {
    const Result<Process>& process = os::process(pid);

    // Ignore any processes that disappear.
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


inline Try<std::set<pid_t> > children(pid_t pid, bool recursive = true)
{
  const Try<std::list<Process> >& processes = os::processes();

  if (processes.isError()) {
    return Error(processes.error());
  }

  return children(pid, processes.get(), recursive);
}


// Overload of os::pids for filtering by groups and sessions.
// A group / session id of 0 will fitler on the group / session ID
// of the calling process.
inline Try<std::set<pid_t> > pids(Option<pid_t> group, Option<pid_t> session)
{
  if (group.isNone() && session.isNone()) {
    return os::pids();
  } else if (group.isSome() && group.get() < 0) {
    return Error("Invalid group");
  } else if (session.isSome() && session.get() < 0) {
    return Error("Invalid session");
  }

  const Try<std::list<Process> >& processes = os::processes();

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


namespace libraries {

// Returns the full library name by adding prefix and extension to
// library name.
inline std::string expandName(const std::string& libraryName)
{
  const char* prefix = "lib";
  const char* extension =
#ifdef __linux__
    ".so";
#else
    ".dylib";
#endif

  return prefix + libraryName + extension;
}


// Returns the current value of LD_LIBRARY_PATH environment variable.
inline std::string paths()
{
  const char* environmentVariable =
#ifdef __linux__
    "LD_LIBRARY_PATH";
#else
    "DYLD_LIBRARY_PATH";
#endif
  return getenv(environmentVariable, false);
}


// Updates the value of LD_LIBRARY_PATH environment variable.
inline void setPaths(const std::string& newPaths)
{
  const char* environmentVariable =
#ifdef __linux__
    "LD_LIBRARY_PATH";
#else
    "DYLD_LIBRARY_PATH";
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
