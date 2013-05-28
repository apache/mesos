#ifndef __STOUT_OS_HPP__
#define __STOUT_OS_HPP__

#ifdef __APPLE__
#include <crt_externs.h> // For _NSGetEnviron().
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <glob.h>
#include <libgen.h>
#include <limits.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <glog/logging.h>

#ifdef __linux__
#include <linux/version.h>
#endif

#include <sys/stat.h>
#include <sys/statvfs.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#include <sys/types.h>
#include <sys/utsname.h>

#include <list>
#include <set>
#include <sstream>
#include <string>

#include "bytes.hpp"
#include "duration.hpp"
#include "error.hpp"
#include "foreach.hpp"
#include "none.hpp"
#include "nothing.hpp"
#include "path.hpp"
#include "result.hpp"
#include "strings.hpp"
#include "try.hpp"

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


inline Try<int> open(const std::string& path, int oflag, mode_t mode = 0)
{
  int fd = ::open(path.c_str(), oflag, mode);

  if (fd < 0) {
    return ErrnoError();
  }

  return fd;
}


inline Try<Nothing> close(int fd)
{
  if (::close(fd) != 0) {
    return ErrnoError();
  }

  return Nothing();
}


inline Try<Nothing> cloexec(int fd)
{
  int flags = ::fcntl(fd, F_GETFD);

  if (flags == -1) {
    return ErrnoError();
  }

  if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
    return ErrnoError();
  }

  return Nothing();
}


inline Try<Nothing> nonblock(int fd)
{
  int flags = ::fcntl(fd, F_GETFL);

  if (flags == -1) {
    return ErrnoError();
  }

  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    return ErrnoError();
  }

  return Nothing();
}


inline Try<bool> isNonblock(int fd)
{
  int flags = ::fcntl(fd, F_GETFL);

  if (flags == -1) {
    return ErrnoError();
  }

  return (flags & O_NONBLOCK) != 0;
}


inline Try<Nothing> touch(const std::string& path)
{
  Try<int> fd =
    open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (fd.isError()) {
    return Error("Failed to open file '" + path + "'");
  }

  // TODO(benh): Is opening/closing sufficient to have the same
  // semantics as the touch utility (i.e., doesn't the utility change
  // the modified date)?
  return close(fd.get());
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
    delete temp;
    return ErrnoError();
  }

  // We ignore the return value of close(). This is because users
  // calling this function are interested in the return value of
  // mkstemp(). Also an unsuccessful close() doesn't affect the file.
  os::close(fd);

  std::string result(temp);
  delete temp;
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
  Try<int> fd = os::open(path, O_WRONLY | O_CREAT | O_TRUNC,
                         S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);
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


// Reads 'size' bytes from a file from its current offset.
// If EOF is encountered before reading size bytes, then the offset
// is restored and none is returned.
inline Result<std::string> read(int fd, size_t size)
{
  // Save the current offset.
  off_t current = lseek(fd, 0, SEEK_CUR);
  if (current == -1) {
    return ErrnoError("Failed to lseek to SEEK_CUR");
  }

  char* buffer = new char[size];
  size_t offset = 0;

  while (offset < size) {
    ssize_t length = ::read(fd, buffer + offset, size - offset);

    if (length < 0) {
      // TODO(bmahler): Handle a non-blocking fd? (EAGAIN, EWOULDBLOCK)
      if (errno == EINTR) {
        continue;
      }
      // Attempt to restore the original offset.
      lseek(fd, current, SEEK_SET);
      return ErrnoError();
    } else if (length == 0) {
      // Reached EOF before expected! Restore the offset.
      lseek(fd, current, SEEK_SET);
      return None();
    }

    offset += length;
  }

  return std::string(buffer, size);
}


// Returns the contents of the file starting from its current offset.
// If an error occurs, this will attempt to recover the file offset.
inline Try<std::string> read(int fd)
{
  // Save the current offset.
  off_t current = lseek(fd, 0, SEEK_CUR);
  if (current == -1) {
    return ErrnoError("Failed to lseek to SEEK_CUR");
  }

  // Get the size of the file from the offset.
  off_t size = lseek(fd, current, SEEK_END);
  if (size == -1) {
    return ErrnoError("Failed to lseek to SEEK_END");
  }

  // Restore the offset.
  if (lseek(fd, current, SEEK_SET) == -1) {
    return ErrnoError("Failed to lseek with SEEK_SET");
  }

  Result<std::string> result = read(fd, size);
  if (result.isNone()) {
    // Hit EOF before reading size bytes.
    return Error("The file size was modified while reading");
  } else if (result.isError()) {
    return Error(result.error());
  }

  return result.get();
}


// A wrapper function that wraps the above read() with
// open and closing the file.
inline Try<std::string> read(const std::string& path)
{
  Try<int> fd =
    os::open(path, O_RDONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (fd.isError()) {
    return Error("Failed to open file '" + path + "'");
  }

  Try<std::string> result = read(fd.get());

  // NOTE: We ignore the return value of close(). This is because users calling
  // this function are interested in the return value of read(). Also an
  // unsuccessful close() doesn't affect the read.
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
    delete temp;
    return ErrnoError();
  }

  std::string s(result);
  delete temp;
  return s;
}


inline Try<std::string> dirname(const std::string& path)
{
  char* temp = new char[path.size() + 1];
  char* result = ::dirname(::strcpy(temp, path.c_str()));
  if (result == NULL) {
    delete temp;
    return ErrnoError();
  }

  std::string s(result);
  delete temp;
  return s;
}


inline Try<std::string> realpath(const std::string& path)
{
  char temp[PATH_MAX];
  if (::realpath(path.c_str(), temp) == NULL) {
    return ErrnoError();
  }
  return std::string(temp);
}


inline bool isdir(const std::string& path)
{
  struct stat s;

  if (::stat(path.c_str(), &s) < 0) {
    return false;
  }
  return S_ISDIR(s.st_mode);
}


inline bool isfile(const std::string& path)
{
  struct stat s;

  if (::stat(path.c_str(), &s) < 0) {
    return false;
  }
  return S_ISREG(s.st_mode);
}


inline bool islink(const std::string& path)
{
  struct stat s;

  if (::lstat(path.c_str(), &s) < 0) {
    return false;
  }
  return S_ISLNK(s.st_mode);
}


inline bool exists(const std::string& path)
{
  struct stat s;

  if (::lstat(path.c_str(), &s) < 0) {
    return false;
  }
  return true;
}


// TODO(benh): Put this in the 'paths' or 'files' or 'fs' namespace.
inline Try<long> mtime(const std::string& path)
{
  struct stat s;

  if (::lstat(path.c_str(), &s) < 0) {
    return ErrnoError("Error invoking stat for '" + path + "'");
  }

  return s.st_mtime;
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
    delete temp;
    return result;
  } else {
    delete temp;
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


inline int system(const std::string& command)
{
  return ::system(command.c_str());
}


// TODO(bmahler): Clean these bool functions to return Try<Nothing>.
// Changes the specified path's user and group ownership to that of
// the specified user..
inline Try<Nothing> chown(
    const std::string& user,
    const std::string& path,
    bool recursive = true)
{
  passwd* passwd;
  if ((passwd = ::getpwnam(user.c_str())) == NULL) {
    return ErrnoError("Failed to get user information for '" + user + "'");
  }

  if (recursive) {
    // TODO(bmahler): Consider walking the file tree instead. We would need
    // to be careful to not miss dotfiles.
    std::string command = "chown -R " + stringify(passwd->pw_uid) + ':' +
      stringify(passwd->pw_gid) + " '" + path + "'";

    int status = os::system(command);
    if (status != 0) {
      return ErrnoError(
          "Failed to execute '" + command +
          "' (exit status: " + stringify(status) + ")");
    }
  } else {
    if (::chown(path.c_str(), passwd->pw_uid, passwd->pw_gid) < 0) {
      return ErrnoError();
    }
  }

  return Nothing();
}


inline bool chmod(const std::string& path, int mode)
{
  if (::chmod(path.c_str(), mode) < 0) {
    PLOG(ERROR) << "Failed to changed the mode of the path '" << path << "'";
    return false;
  }

  return true;
}


inline bool chdir(const std::string& directory)
{
  if (::chdir(directory.c_str()) < 0) {
    PLOG(ERROR) << "Failed to change directory";
    return false;
  }

  return true;
}


inline bool su(const std::string& user)
{
  passwd* passwd;
  if ((passwd = ::getpwnam(user.c_str())) == NULL) {
    PLOG(ERROR) << "Failed to get user information for '"
                << user << "', getpwnam";
    return false;
  }

  if (::setgid(passwd->pw_gid) < 0) {
    PLOG(ERROR) << "Failed to set group id, setgid";
    return false;
  }

  if (::setuid(passwd->pw_uid) < 0) {
    PLOG(ERROR) << "Failed to set user id, setuid";
    return false;
  }

  return true;
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


// TODO(bmahler): Wrap with a Try.
inline std::list<std::string> ls(const std::string& directory)
{
  std::list<std::string> result;

  DIR* dir = opendir(directory.c_str());

  if (dir == NULL) {
    return std::list<std::string>();
  }

  // Calculate the size for a "directory entry".
  long name_max = fpathconf(dirfd(dir), _PC_NAME_MAX);

  // If we don't get a valid size, check NAME_MAX, but fall back on
  // 255 in the worst case ... Danger, Will Robinson!
  if (name_max == -1) {
    name_max = (NAME_MAX > 255) ? NAME_MAX : 255;
  }

  size_t name_end =
    (size_t) offsetof(dirent, d_name) + name_max + 1;

  size_t size = (name_end > sizeof(dirent)
    ? name_end
    : sizeof(dirent));

  dirent* temp = (dirent*) malloc(size);

  if (temp == NULL) {
    free(temp);
    closedir(dir);
    return std::list<std::string>();
  }

  struct dirent* entry;

  int error;

  while ((error = readdir_r(dir, temp, &entry)) == 0 && entry != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    result.push_back(entry->d_name);
  }

  free(temp);
  closedir(dir);

  if (error != 0) {
    return std::list<std::string>();
  }

  return result;
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

  if (!isdir(directory)) {
    return Error("'" + directory + "' is not a directory");
  }

  foreach (const std::string& entry, ls(directory)) {
    std::string path = path::join(directory, entry);
    // If it's a directory, recurse.
    if (isdir(path) && !islink(path)) {
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

  return results;
}


inline std::string user()
{
  passwd* passwd;
  if ((passwd = getpwuid(getuid())) == NULL) {
    LOG(FATAL) << "Failed to get username information";
  }

  return passwd->pw_name;
}


inline Try<std::string> hostname()
{
  char host[512];

  if (gethostname(host, sizeof(host)) < 0) {
    return ErrnoError();
  }

  // Allocate temporary buffer for gethostbyname2_r.
  size_t length = 1024;
  char* temp = new char[length];

  struct hostent he, *hep = NULL;
  int result = 0;
  int herrno = 0;

  while ((result = gethostbyname2_r(host, AF_INET, &he, temp,
                                    length, &hep, &herrno)) == ERANGE) {
    // Enlarge the buffer.
    delete[] temp;
    length *= 2;
    temp = new char[length];
  }

  if (result != 0 || hep == NULL) {
    delete[] temp;
    return Error(hstrerror(herrno));
  }

  std::string hostname = hep->h_name;
  delete[] temp;
  return Try<std::string>::some(hostname);
}


// Runs a shell command formatted with varargs and return the return value
// of the command. Optionally, the output is returned via an argument.
// TODO(vinod): Pass an istream object that can provide input to the command.
inline Try<int> shell(std::ostream* os, const std::string& fmt, ...)
{
  va_list args;
  va_start(args, fmt);

  const Try<std::string>& cmdline = strings::internal::format(fmt, args);

  va_end(args);

  if (cmdline.isError()) {
    return Error(cmdline.error());
  }

  FILE* file;

  if ((file = popen(cmdline.get().c_str(), "r")) == NULL) {
    return Error("Failed to run '" + cmdline.get() + "'");
  }

  char line[1024];
  // NOTE(vinod): Ideally the if and while loops should be interchanged. But
  // we get a broken pipe error if we don't read the output and simply close.
  while (fgets(line, sizeof(line), file) != NULL) {
    if (os != NULL) {
      *os << line ;
    }
  }

  if (ferror(file) != 0) {
    ErrnoError error("Error reading output of '" + cmdline.get() + "'");
    pclose(file); // Ignoring result since we already have an error.
    return error;
  }

  int status;
  if ((status = pclose(file)) == -1) {
    return Error("Failed to get status of '" + cmdline.get() + "'");
  }

  return status;
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


// Returns the total size of main memory.
inline Try<Bytes> memory()
{
#ifdef __linux__
  struct sysinfo info;
  if (sysinfo(&info) != 0) {
    return ErrnoError();
  }
# if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 3, 23)
  return Bytes(info.totalram * info.mem_unit);
# else
  return Bytes(info.totalram);
# endif
#elif defined __APPLE__
  const size_t NAME_SIZE = 2;
  int name[NAME_SIZE];
  name[0] = CTL_HW;
  name[1] = HW_MEMSIZE;

  int64_t memory;
  size_t length = sizeof(memory);

  if (sysctl(name, NAME_SIZE, &memory, &length, NULL, 0) < 0) {
    return ErrnoError("Failed to get sysctl of HW_MEMSIZE");
  }
  return Bytes(memory);
#else
  return Error("Cannot determine the size of main memory");
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


// The OS release level.
struct Release
{
  int version;
  int major;
  int minor;
};


// Return the OS release numbers.
inline Try<Release> release()
{
  Try<UTSInfo> info = uname();
  if (info.isError()) {
    return Error(info.error());
  }

  Release r;
  if (::sscanf(
          info.get().release.c_str(),
          "%d.%d.%d",
          &r.version,
          &r.major,
          &r.minor) != 3) {
    return Error("Failed to parse: " + info.get().release);
  }

  return r;
}


inline Try<bool> alive(pid_t pid)
{
  CHECK(pid > 0);

  if (::kill(pid, 0) == 0) {
    return true;
  }

  if (errno == ESRCH) {
    return false;
  }

  return Try<bool>::error(strerror(errno));
}

} // namespace os {

#endif // __STOUT_OS_HPP__
