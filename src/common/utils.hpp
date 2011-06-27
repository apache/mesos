#ifndef __UTILS_HPP__
#define __UTILS_HPP__

#include <errno.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <netdb.h>
#include <pwd.h>
#include <stddef.h>
#include <unistd.h>

#include <google/protobuf/message.h>

#include <glog/logging.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <list>

#include <boost/lexical_cast.hpp>

#include "common/foreach.hpp"
#include "common/result.hpp"
#include "common/tokenize.hpp"

#ifdef __APPLE__
#define gethostbyname2_r(name, af, ret, buf, buflen, result, h_errnop)  \
  ({ *(result) = gethostbyname2(name, af); 0; })
#endif // __APPLE__


namespace mesos { namespace internal { namespace utils {

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

namespace protobuf { 

// Write out the given protobuf to the specified file descriptor by
// first writing out the length of the protobuf followed by the
// contents.
inline bool write(int fd, const google::protobuf::Message& message)
{
  if (!message.IsInitialized()) {
    LOG(ERROR) << "Failed to write protocol buffer to file, "
               << "protocol buffer is not initialized!";
    return false;
  }

  uint32_t size = message.ByteSize();
  
  ssize_t length = ::write(fd, (void*) &size, sizeof(size));

  if (length != sizeof(size)) {
    PLOG(ERROR) << "Failed to write protocol buffer to file, write";
    return false;
  }

  return message.SerializeToFileDescriptor(fd);
}


// Read the next protobuf from the file by first reading the "size"
// followed by the contents (as written by 'write' above).
inline bool read(int fd, google::protobuf::Message* message)
{
  if (message == NULL) {
    return false;
  }

  // Save the offset so we can re-adjust if something goes wrong.
  off_t offset = lseek(fd, 0, SEEK_CUR);

  if (offset < 0) {
    return false;
  }

  uint32_t size;
  ssize_t length = ::read(fd, (void*) &size, sizeof(size));

  if (length != sizeof(size)) {
    PLOG(ERROR) << "Failed to read protocol buffer from file, read";

    // Return the file position.

    lseek(fd, offset, SEEK_SET);
    return false;
  }

  char* temp = new char[size];

  length = ::read(fd, temp, size);

  if (length != size) {
    PLOG(ERROR) << "Failed to read protocol buffer from file, read";

    // Return the file position.
    lseek(fd, offset, SEEK_SET);

    return false;
  }

  google::protobuf::io::ArrayInputStream stream(temp, length);
  bool result = message->ParseFromZeroCopyStream(&stream);

  delete[] temp;

  return result;
}

} // namespace protobuf {


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

  return std::string(value);
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


inline std::string basename(const std::string& path)
{
  return ::basename(const_cast<char*>(path.c_str()));
}


inline bool mkdir(const std::string& directory)
{
  try {
    std::vector<std::string> tokens = tokenize::split(directory, "/");

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


// Changes the specified file's user and group ownership to that of
// the specified user..
inline bool chown(const std::string& user, const std::string& file)
{
  struct passwd* passwd;
  if ((passwd = ::getpwnam(user.c_str())) == NULL) {
    PLOG(ERROR) << "Failed to get user information for '"
                << user
                << "', getpwnam";
    return false;
  }

  if (::chown(file.c_str(), passwd->pw_uid, passwd->pw_gid) < 0) {
    PLOG(ERROR) << "Failed to change file user and group ownership, chown";
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
  struct passwd* passwd;
  if ((passwd = ::getpwnam(user.c_str())) == NULL) {
    PLOG(ERROR) << "Failed to get user information for '"
                << user
                << "', getpwnam";
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
      delete[] temp;
      if (errno != ERANGE) {
        return std::string();
      }
      size *= 2;
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

} // namespace os {

}}} // namespace mesos { namespace internal { namespace utils {

#endif // __UTILS_HPP__
