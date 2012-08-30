#ifndef __STOUT_OS_HPP__
#define __STOUT_OS_HPP__

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <libgen.h>
#include <limits.h>
#include <netdb.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glog/logging.h>

#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#include <sys/types.h>
#include <sys/utsname.h>

#include <list>
#include <string>

#include "foreach.hpp"
#include "result.hpp"
#include "strings.hpp"
#include "try.hpp"

#ifdef __APPLE__
// Assigning the result pointer to ret silences an unused var warning.
#define gethostbyname2_r(name, af, ret, buf, buflen, result, h_errnop)  \
  ({ (void)ret; *(result) = gethostbyname2(name, af); 0; })
#endif // __APPLE__

namespace os {

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


inline Try<int> open(const std::string& path, int oflag, mode_t mode = 0)
{
  int fd = ::open(path.c_str(), oflag, mode);

  if (fd < 0) {
    return Try<int>::error(strerror(errno));
  }

  return Try<int>::some(fd);
}


inline Try<bool> close(int fd)
{
  if (::close(fd) != 0) {
    return Try<bool>::error(strerror(errno));
  }

  return true;
}


inline Try<bool> cloexec(int fd)
{
  int flags = ::fcntl(fd, F_GETFD);
  if (flags == -1) {
    return Try<bool>::error(strerror(errno));
  }
  if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
    return Try<bool>::error(strerror(errno));;
  }
  return true;
}


inline Try<bool> nonblock(int fd)
{
  int flags = ::fcntl(fd, F_GETFL);

  if (flags == -1) {
    return Try<bool>::error(strerror(errno));
  }

  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    return Try<bool>::error(strerror(errno));
  }

  return true;
}


inline Try<bool> isNonblock(int fd)
{
  int flags = ::fcntl(fd, F_GETFL);

  if (flags == -1) {
    return Try<bool>::error(strerror(errno));
  }

  return (flags & O_NONBLOCK) != 0;
}


inline Try<bool> touch(const std::string& path)
{
  Try<int> fd = open(path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (fd.isError()) {
    return Try<bool>::error("Failed to open file " + path);
  }

  // TODO(benh): Is opening/closing sufficient to have the same
  // semantics as the touch utility (i.e., doesn't the utility change
  // the modified date)?

  Try<bool> result = close(fd.get());

  if (result.isError()) {
    return Try<bool>::error("Failed to close file " + path);
  }

  return true;
}


// Write out the string to the file at the current fd position.
inline Try<bool> write(int fd, const std::string& message)
{
  ssize_t length = ::write(fd, message.data(), message.length());

  if (length == -1) {
    return Try<bool>::error(strerror(errno));
  }

  CHECK(length > 0);
  // TODO(benh): Handle a non-blocking fd?
  CHECK(static_cast<size_t>(length) == message.length());

  return true;
}


// A wrapper function that wraps the above write() with
// open and closing the file.
inline Try<bool> write(const std::string& path,
                       const std::string& message)
{
  Try<int> fd = os::open(path, O_WRONLY | O_CREAT | O_TRUNC,
                         S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);
  if (fd.isError()) {
    return Try<bool>::error("Failed to open file " + path);
  }

  Try<bool> result = write(fd.get(), message);

  // NOTE: We ignore the return value of close(). This is because users calling
  // this function are interested in the return value of write(). Also an
  // unsuccessful close() doesn't affect the write.
  os::close(fd.get());

  return result;
}


// Read the contents of the file from its current offset
// and return it as a string.
inline Result<std::string> read(int fd)
{
  // Get the size of the file.
  off_t offset = lseek(fd, 0, SEEK_CUR);
  if (offset == -1) {
    return Result<std::string>::error("Error seeking to SEEK_CUR");
  }

  off_t size = lseek(fd, 0, SEEK_END);
  if (size == -1) {
      return Result<std::string>::error("Error seeking to SEEK_END");
  }

  if (lseek(fd, offset, SEEK_SET) == -1) {
    return Result<std::string>::error("Error seeking to SEEK_SET");
  }

  // Allocate memory.
  char* buffer = new char[size];

  ssize_t length = ::read(fd, buffer, size);

  if (length == 0) {
    return Result<std::string>::none();
  } else if (length == -1) {
    // Save the error, reset the file offset, and return the error.
    return Result<std::string>::error(strerror(errno));
  } else if (length != size) {
    return Result<std::string>::error("Couldn't read the entire file");
  }

  std::string result(buffer, size);

  return result;
}


// A wrapper function that wraps the above read() with
// open and closing the file.
inline Result<std::string> read(const std::string& path)
{
  Try<int> fd = os::open(path, O_RDONLY,
                         S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (fd.isError()) {
    return Result<std::string>::error("Failed to open file " + path);
  }

  Result<std::string> result = read(fd.get());

  // NOTE: We ignore the return value of close(). This is because users calling
  // this function are interested in the return value of read(). Also an
  // unsuccessful close() doesn't affect the read.
  os::close(fd.get());

  return result;
}


inline Try<bool> rm(const std::string& path)
{
  if (::remove(path.c_str()) != 0) {
    return Try<bool>::error(strerror(errno));
  }

  return true;
}


inline std::string basename(const std::string& path)
{
  return ::basename(const_cast<char*>(path.c_str()));
}


inline std::string dirname(const std::string& path)
{
  return ::dirname(const_cast<char*>(path.c_str()));
}


inline Try<std::string> realpath(const std::string& path)
{
  char temp[PATH_MAX];
  if (::realpath(path.c_str(), temp) == NULL) {
    // TODO(benh): Include strerror(errno).
    return Try<std::string>::error(
      "Failed to canonicalize " + path + " into an absolute path");
  }
  return std::string(temp);
}


inline bool exists(const std::string& path, bool directory = false)
{
  struct stat s;

  if (::stat(path.c_str(), &s) < 0) {
    return false;
  }

  // Check if it's a directory if requested.
  return directory ? S_ISDIR(s.st_mode) : true;
}


// TODO(benh): Put this in the 'paths' or 'files' or 'fs' namespace.
inline Try<long> mtime(const std::string& path)
{
  struct stat s;

  if (::stat(path.c_str(), &s) < 0) {
    return Try<long>::error(strerror(errno));
  }

  return s.st_mtime;
}


inline bool mkdir(const std::string& directory)
{
  try {
    std::vector<std::string> tokens = strings::split(directory, "/");

    std::string path = "";

    // We got an absolute path, so keep the leading slash.
    if (directory.find_first_of("/") == 0) {
      path = "/";
    }

    foreach (const std::string& token, tokens) {
      path += token;
      if (::mkdir(path.c_str(), 0755) < 0 && errno != EEXIST) {
        PLOG(ERROR) << "Failed to make directory, mkdir";
        return false;
      }
      path += "/";
    }
  } catch (...) {
    return false;
  }

  return true;
}


// Recursively deletes a directory akin to: 'rm -r'. Note that this
// function expects an absolute path.
inline bool rmdir(const std::string& directory)
{
  char* paths[] = {const_cast<char*>(directory.c_str()), NULL};

  FTS* tree = fts_open(paths, FTS_NOCHDIR, NULL);
  if (tree == NULL) {
    return false;
  }

  FTSENT* node;
  while ((node = fts_read(tree)) != NULL) {
    switch (node->fts_info) {
      case FTS_DP:
        ::rmdir(node->fts_path);
        break;
      case FTS_F:
      case FTS_SL:
        ::unlink(node->fts_path);
        break;
      default:
        break;
    }
  }

  if (errno != 0) {
    return false;
  }

  return fts_close(tree) == 0;
}


// Changes the specified path's user and group ownership to that of
// the specified user..
inline bool chown(const std::string& user, const std::string& path)
{
  passwd* passwd;
  if ((passwd = ::getpwnam(user.c_str())) == NULL) {
    PLOG(ERROR) << "Failed to get user information for '"
                << user << "', getpwnam";
    return false;
  }

  if (::chown(path.c_str(), passwd->pw_uid, passwd->pw_gid) < 0) {
    PLOG(ERROR) << "Failed to change user and group ownership of '"
                << path << "', chown";
    return false;
  }

  return true;
}


inline bool chmod(const std::string& path, int mode)
{
  if (::chmod(path.c_str(), mode) < 0) {
    PLOG(ERROR) << "Failed to changed the mode of the path " << path
                << " due to " << strerror(errno);
    return false;
  }

  return true;
}


inline bool chdir(const std::string& directory)
{
  if (chdir(directory.c_str()) < 0) {
    PLOG(ERROR) << "Failed to change directory, chdir";
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
// TODO(vinod): Support regular expressions for pattern.
// TODO(vinod): Consider using ftw or a non-recursive approach.
inline Try<std::list<std::string> > find(const std::string& directory,
                                         const std::string& pattern)
{
  std::list<std::string> results;

  if (!exists(directory, true)) {
    return Try<std::list<std::string> >::error("Directory " + directory + " doesn't exist!");
  }

  foreach (const std::string& entry, ls(directory)) {
    if (entry == "." || entry == "..") {
      continue;
    }
    std::string result = directory + '/' + entry;
    // This is just a hack to check whether this path is a regular file or
    // a (sub) directory.
    if (exists(result, true)) { // If its a directory recurse.
      CHECK(find(result, pattern).isSome()) << "Directory " << directory << " doesn't exist";
      foreach (const std::string& path, find(result, pattern).get()) {
        results.push_back(path);
      }
    } else {
      if (basename(result).find(pattern) != std::string::npos) {
        results.push_back(result); // Matched the file pattern!
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
    return Try<std::string>::error(strerror(errno));
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
    return Try<std::string>::error(hstrerror(herrno));
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
    return Try<int>::error(cmdline.error());
  }

  FILE* file;

  if ((file = popen(cmdline.get().c_str(), "r")) == NULL) {
    return Try<int>::error("Failed to run '" + cmdline.get() + "'");
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
    const std::string& error =
      "Error reading output of '" + cmdline.get() + "': " + strerror(errno);
    pclose(file); // Ignoring result since we already have an error.
    return Try<int>::error(error);
  }

  int status;
  if ((status = pclose(file)) == -1) {
    return Try<int>::error("Failed to get status of '" + cmdline.get() + "'");
  }

  return status;
}


inline int system(const std::string& command)
{
  return ::system(command.c_str());
}


// Returns the total number of cpus (cores).
inline Try<long> cpus()
{
  return sysconf(_SC_NPROCESSORS_ONLN);
}


// Returns the total size of main memory in bytes.
inline Try<long> memory()
{
#ifdef __linux__
  struct sysinfo info;
  if (sysinfo(&info) != 0) {
    return Try<long>::error(strerror(errno));
  }
  return info.totalram;
#else
  return Try<long>::error("Cannot determine the size of main memory");
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
    return Try<UTSInfo>::error(
        "Failed to get system information: " + std::string(strerror(errno)));
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
    return Try<std::string>::error(info.error());
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
    return Try<Release>::error(info.error());
  }

  Release r;
  if (::sscanf(info.get().release.c_str(), "%d.%d.%d",
               &r.version, &r.major, &r.minor) != 3) {
    return Try<Release>::error(
        "Parsing release number error: " + info.get().release);
  }

  return r;
}


} // namespace os {

#endif // __STOUT_OS_HPP__
