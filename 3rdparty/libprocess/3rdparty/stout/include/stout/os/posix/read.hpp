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
#ifndef __STOUT_OS_POSIX_READ_HPP__
#define __STOUT_OS_POSIX_READ_HPP__

#include <stdio.h>
#include <unistd.h>

#include <string>

#ifdef __sun
#include <fstream>
#endif // __sun

#include <stout/error.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>


namespace os {

// Reads 'size' bytes from a file from its current offset.
// If EOF is encountered before reading 'size' bytes then the result
// will contain the bytes read and a subsequent read will return None.
inline Result<std::string> read(int fd, size_t size)
{
  char* buffer = new char[size];
  size_t offset = 0;

  while (offset < size) {
    ssize_t length = ::read(fd, buffer + offset, size - offset);

    if (length < 0) {
      // TODO(bmahler): Handle a non-blocking fd? (EAGAIN, EWOULDBLOCK)
      if (errno == EINTR) {
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


// Returns the contents of the file.
#ifdef __sun // getline is not available on Solaris, using STL.
inline Try<std::string> read(const std::string& path)
{
  std::ifstream ifs(path.c_str());
  if (!ifs.is_open()) {
    return ErrnoError("Failed to open file '" + path + "'");
  }
  return std::string((std::istreambuf_iterator<char>(ifs)),
                     (std::istreambuf_iterator<char>()));
}
#else
inline Try<std::string> read(const std::string& path)
{
  FILE* file = fopen(path.c_str(), "r");
  if (file == NULL) {
    return ErrnoError("Failed to open file '" + path + "'");
  }

  // Initially the 'line' is NULL and length 0, getline() allocates
  // ('malloc') a buffer for reading the line.
  // In subsequent iterations, if the buffer is not large enough to
  // hold the line, getline() resizes it with 'realloc' and updates
  // 'line' and 'length' as necessary. See:
  // - http://pubs.opengroup.org/onlinepubs/9699919799/functions/getline.html
  // - http://man7.org/linux/man-pages/man3/getline.3.html
  std::string result;
  char* line = NULL;
  size_t length = 0;
  ssize_t read;

  while ((read = getline(&line, &length, file)) != -1) {
    result.append(line, read);
  }

  // getline() requires the line buffer to be freed by the caller.
  free(line);

  if (ferror(file)) {
    ErrnoError error;
    // NOTE: We ignore the error from fclose(). This is because
    // users calling this function are interested in the return value
    // of read(). Also an unsuccessful fclose() does not affect the
    // read.
    fclose(file);
    return error;
  }

  fclose(file);
  return result;
}
#endif // __sun

} // namespace os {

#endif // __STOUT_OS_POSIX_READ_HPP__
