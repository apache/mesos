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

#ifndef __UTILS_HPP__
#define __UTILS_HPP__

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

#include <google/protobuf/message.h>

#include <glog/logging.h>

#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#include <sys/types.h>

#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <list>
#include <set>

#include <boost/lexical_cast.hpp>

#include "common/foreach.hpp"
#include "common/logging.hpp"
#include "common/option.hpp"
#include "common/result.hpp"
#include "common/strings.hpp"
#include "common/try.hpp"

#ifdef __APPLE__
#define gethostbyname2_r(name, af, ret, buf, buflen, result, h_errnop)  \
  ({ *(result) = gethostbyname2(name, af); 0; })
#endif // __APPLE__


namespace mesos {
namespace internal {
namespace utils {

template <typename T>
T copy(const T& t) { return t; }


template <typename T>
std::string stringify(T t)
{
  try {
    return boost::lexical_cast<std::string>(t);
  } catch (const boost::bad_lexical_cast&) {
    LOG(FATAL) << "Failed to stringify!";
  }
}


template <typename T>
std::string stringify(const std::set<T>& set)
{
  std::ostringstream out;
  out << "{ ";
  typename std::set<T>::const_iterator iterator = set.begin();
  while (iterator != set.end()) {
    out << utils::stringify(*iterator);
    if (++iterator != set.end()) {
      out << ", ";
    }
  }
  out << " }";
  return out.str();
}


template <typename T>
Try<T> numify(const std::string& s)
{
  try {
    return boost::lexical_cast<T>(s);
  } catch (const boost::bad_lexical_cast&) {
    const Try<std::string>& message = strings::format(
        "Failed to convert '%s' to number", s.c_str());
    return Try<T>::error(
        message.isSome() ? message.get() : "Failed to convert to number");
  }
}


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


// Sets the value associated with the specfied key in the set of
// environment variables.
inline void setenv(const std::string& key,
                   const std::string& value,
                   bool overwrite = true)
{
  ::setenv(key.c_str(), value.c_str(), overwrite ? 1 : 0);
}


// Unsets the value associated with the specfied key in the set of
// environment variables.
inline void unsetenv(const std::string& key)
{
  ::unsetenv(key.c_str());
}


inline Result<int> open(const std::string& path, int oflag, mode_t mode = 0)
{
  int fd = ::open(path.c_str(), oflag, mode);

  if (fd < 0) {
    return Result<int>::error(strerror(errno));
  }

  return Result<int>::some(fd);
}


inline Result<bool> close(int fd)
{
  if (::close(fd) != 0) {
    return Result<bool>::error(strerror(errno));
  }

  return true;
}


inline Result<bool> rm(const std::string& path)
{
  if (::remove(path.c_str()) != 0) {
    return Result<bool>::error(strerror(errno));
  }

  return true;
}


inline std::string basename(const std::string& path)
{
  return ::basename(const_cast<char*>(path.c_str()));
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
bool rmdir(const std::string& directory);


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


inline std::list<std::string> listdir(const std::string& directory)
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
    result.push_back(entry->d_name);
  }

  free(temp);
  closedir(dir);

  if (error != 0) {
    return std::list<std::string>();
  }

  return result;
}


inline std::string user()
{
  passwd* passwd;
  if ((passwd = getpwuid(getuid())) == NULL) {
    LOG(FATAL) << "Failed to get username information";
  }

  return passwd->pw_name;
}


inline Result<std::string> hostname()
{
  char host[512];

  if (gethostname(host, sizeof(host)) < 0) {
    return Result<std::string>::error(strerror(errno));
  }

  struct hostent he, *hep;
  char* temp;
  size_t length;
  int result;
  int herrno;

  // Allocate temporary buffer for gethostbyname2_r.
  length = 1024;
  temp = new char[length];

  while ((result = gethostbyname2_r(host, AF_INET, &he, temp,
            length, &hep, &herrno)) == ERANGE) {
    // Enlarge the buffer.
    delete[] temp;
    length *= 2;
    temp = new char[length];
  }

  if (result != 0 || hep == NULL) {
    delete[] temp;
    return Result<std::string>::error(hstrerror(herrno));
  }

  std::string hostname = hep->h_name;
  delete[] temp;
  return Result<std::string>::some(hostname);
}


// Runs a shell command formatted with varargs and return the return value
// of the command. Optionally, the output is returned via an argument.
inline Try<int> shell(std::iostream* ios, const std::string& fmt, ...)
{
  va_list args;
  va_start(args, fmt);

  const Try<std::string>& cmdline = strings::format(fmt, args);

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
    if (ios != NULL) {
      *ios << line ;
    }
  }

  if (ferror(file) != 0) {
    std::string error =
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

} // namespace os {


namespace protobuf {

// Write out the given protobuf to the specified file descriptor by
// first writing out the length of the protobuf followed by the
// contents.
inline Result<bool> write(int fd, const google::protobuf::Message& message)
{
  if (!message.IsInitialized()) {
    LOG(ERROR) << "Failed to write protocol buffer to file, "
               << "protocol buffer is not initialized!";
    return false;
  }

  uint32_t size = message.ByteSize();

  ssize_t length = ::write(fd, (void*) &size, sizeof(size));

  if (length == -1) {
    std::string error = strerror(errno);
    error = error + " (" + __FILE__ + ":" + utils::stringify(__LINE__) + ")";
    return Result<bool>::error(error);
  }

  CHECK(length != 0);
  CHECK(length == sizeof(size)); // TODO(benh): Handle a non-blocking fd?

  return message.SerializeToFileDescriptor(fd);
}


// A wrapper function that wraps the above write() with
// open and closing the file.
inline Result<bool> write(const std::string& path,
                          const google::protobuf::Message& message)
{
  Result<int> fd = os::open(path, O_WRONLY | O_CREAT | O_TRUNC,
                            S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);
  if (fd.isError()) {
    LOG(ERROR) << "Failed to open file: " << path;
    return Result<bool>::error("Failed to open file.");
  }

  Result<bool> result = write(fd.get(), message);

  // NOTE: We ignore the return value of close(). This is because users calling
  // this function are interested in the return value of write(). Also an
  // unsuccessful close() doesn't affect the write.
  os::close(fd.get());

  return result;
}


// Read the next protobuf from the file by first reading the "size"
// followed by the contents (as written by 'write' above).
inline Result<bool> read(int fd, google::protobuf::Message* message)
{
  CHECK(message != NULL);

  message->Clear();

  // Save the offset so we can re-adjust if something goes wrong.
  off_t offset = lseek(fd, 0, SEEK_CUR);

  if (offset < 0) {
    std::string error = strerror(errno);
    error = error + " (" + __FILE__ + ":" + utils::stringify(__LINE__) + ")";
    return Result<bool>::error(error);
  }

  uint32_t size;
  ssize_t length = ::read(fd, (void*) &size, sizeof(size));

  if (length == 0) {
    return Result<bool>::none();
  } else if (length == -1) {
    // Save the error, reset the file offset, and return the error.
    std::string error = strerror(errno);
    error = error + " (" + __FILE__ + ":" + utils::stringify(__LINE__) + ")";
    lseek(fd, offset, SEEK_SET);
    return Result<bool>::error(error);
  } else if (length != sizeof(size)) {
    return false;
  }

  // TODO(benh): Use a different format for writing a protobuf to disk
  // so that we don't need to have broken heuristics like this!
  if (size > 10 * 1024 * 1024) { // 10 MB
    // Save the error, reset the file offset, and return the error.
    std::string error = "Size > 10 MB, possible corruption detected";
    error = error + " (" + __FILE__ + ":" + utils::stringify(__LINE__) + ")";
    lseek(fd, offset, SEEK_SET);
    return Result<bool>::error(error);;
  }

  char* temp = new char[size];

  length = ::read(fd, temp, size);

  if (length == 0) {
    delete[] temp;
    return Result<bool>::none();
  } else if (length == -1) {
    // Save the error, reset the file offset, and return the error.
    std::string error = strerror(errno);
    error = error + " (" + __FILE__ + ":" + utils::stringify(__LINE__) + ")";
    lseek(fd, offset, SEEK_SET);
    delete[] temp;
    return Result<bool>::error(error);
  } else if (length != size) {
    delete[] temp;
    return false;
  }

  google::protobuf::io::ArrayInputStream stream(temp, length);
  bool parsed = message->ParseFromZeroCopyStream(&stream);

  delete[] temp;

  if (!parsed) {
    // Save the error, reset the file offset, and return the error.
    std::string error = "Failed to parse protobuf";
    error = error + " (" + __FILE__ + ":" + utils::stringify(__LINE__) + ")";
    lseek(fd, offset, SEEK_SET);
    return Result<bool>::error(error);;
  }

  return true;
}


// A wrapper function that wraps the above read() with
// open and closing the file.
inline Result<bool> read(const std::string& path,
                         google::protobuf::Message* message)
{
  Result<int> fd = os::open(path, O_RDONLY,
                            S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (fd.isError()) {
    LOG(ERROR) << "Failed to open file: " << path;
    return Result<bool>::error("Failed to open file.");
  }

  Result<bool> result = read(fd.get(), message);

  // NOTE: We ignore the return value of close(). This is because users calling
  // this function are interested in the return value of read(). Also an
  // unsuccessful close() doesn't affect the read.
  os::close(fd.get());

  return result;
}


} // namespace protobuf {

// Handles http requests.
namespace net {

// Returns the return code resulting from attempting to download the
// specified HTTP or FTP URL into a file at the specified path.
inline Try<int> download(const std::string& url, const std::string& path)
{
#ifndef HAVE_LIBCURL
  return Try<int>::error("Downloading via HTTP/FTP is not supported");
#else
  Result<int> fd = utils::os::open(path, O_CREAT | O_WRONLY,
                                   S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  CHECK(!fd.isNone());

  if (fd.isError()) {
    return Try<int>::error(fd.error());
  }

  curl_global_init(CURL_GLOBAL_ALL);
  CURL* curl = curl_easy_init();

  if (curl == NULL) {
    curl_easy_cleanup(curl);
    utils::os::close(fd.get());
    return Try<int>::error("Failed to initialize libcurl");
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);

  FILE* file = fdopen(fd.get(), "w");
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

  CURLcode curlErrorCode = curl_easy_perform(curl);
  if (curlErrorCode != 0) {
    curl_easy_cleanup(curl);
    fclose(file);
    return Try<int>::error(curl_easy_strerror(curlErrorCode));
  }

  long code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);

  if (!fclose(file)) {
    return Try<int>::error("Failed to close file handle");
  }

  return Try<int>::some(code);
#endif // HAVE_LIBCURL
}

} // namespace net {

} // namespace utils {
} // namespace internal {
} // namespace mesos {

#endif // __UTILS_HPP__
