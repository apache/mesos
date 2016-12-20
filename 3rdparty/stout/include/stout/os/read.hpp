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

#ifndef __STOUT_OS_READ_HPP__
#define __STOUT_OS_READ_HPP__

#include <assert.h>
#include <stdio.h>
#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#include <string>

#if defined(__sun) || defined(__WINDOWS__)
#include <fstream>
#endif // __sun || __WINDOWS__

#include <stout/error.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>
#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__

#include <stout/os/int_fd.hpp>
#include <stout/os/socket.hpp>

#ifdef __WINDOWS__
#include <stout/os/windows/read.hpp>
#else
#include <stout/os/posix/read.hpp>
#endif // __WINDOWS__


namespace os {

// Reads 'size' bytes from a file from its current offset.
// If EOF is encountered before reading 'size' bytes then the result
// will contain the bytes read and a subsequent read will return None.
inline Result<std::string> read(int_fd fd, size_t size)
{
  char* buffer = new char[size];
  size_t offset = 0;

  while (offset < size) {
    ssize_t length = os::read(fd, buffer + offset, size - offset);

#ifdef __WINDOWS__
      int error = WSAGetLastError();
#else
      int error = errno;
#endif // __WINDOWS__

    if (length < 0) {
      // TODO(bmahler): Handle a non-blocking fd? (EAGAIN, EWOULDBLOCK)
      if (net::is_restartable_error(error)) {
        continue;
      }
      ErrnoError error; // Constructed before 'delete' to capture errno.
      delete[] buffer;
      return error;
    } else if (length == 0) {
      // Reached EOF before expected! Only return as much data as
      // available or None if we haven't read anything yet.
      if (offset > 0) {
        std::string result(buffer, offset);
        delete[] buffer;
        return result;
      }
      delete[] buffer;
      return None();
    }

    offset += length;
  }

  std::string result(buffer, size);
  delete[] buffer;
  return result;
}


// Returns the contents of the file. NOTE: getline is not available on Solaris
// or Windows, so we use STL.
#if defined(__sun) || defined(__WINDOWS__)
inline Try<std::string> read(const std::string& path)
{
  std::ifstream file(path.c_str());
  if (!file.is_open()) {
    // Does ifstream actually set errno?
    return ErrnoError("Failed to open file");
  }
  return std::string((std::istreambuf_iterator<char>(file)),
                     (std::istreambuf_iterator<char>()));
}
#else
inline Try<std::string> read(const std::string& path)
{
  FILE* file = ::fopen(path.c_str(), "r");
  if (file == nullptr) {
    return ErrnoError("Failed to open file");
  }

  // Use a buffer to read the file in BUFSIZ
  // chunks and append it to the string we return.
  //
  // NOTE: We aren't able to use fseek() / ftell() here
  // to find the file size because these functions don't
  // work properly for in-memory files like /proc/*/stat.
  char* buffer = new char[BUFSIZ];
  std::string result;

  while (true) {
    size_t read = ::fread(buffer, 1, BUFSIZ, file);

    if (::ferror(file)) {
      // NOTE: ferror() will not modify errno if the stream
      // is valid, which is the case here since it is open.
      ErrnoError error;
      delete[] buffer;
      ::fclose(file);
      return error;
    }

    result.append(buffer, read);

    if (read != BUFSIZ) {
      assert(feof(file));
      break;
    }
  };

  ::fclose(file);
  delete[] buffer;
  return result;
}
#endif // __sun || __WINDOWS__

} // namespace os {

#endif // __STOUT_OS_READ_HPP__
