#ifndef __STOUT_OS_READ_HPP__
#define __STOUT_OS_READ_HPP__

#include <stdio.h>
#include <unistd.h>

#include <stout/error.hpp>
#include <stout/try.hpp>

namespace os {

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
      delete[] buffer;
      return ErrnoError();
    } else if (length == 0) {
      // Reached EOF before expected! Restore the offset.
      lseek(fd, current, SEEK_SET);
      delete[] buffer;
      return None();
    }

    offset += length;
  }

  std::string result = std::string(buffer, size);
  delete[] buffer;
  return result;
}


// Returns the contents of the file.
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

} // namespace os {

#endif // __STOUT_OS_READ_HPP__
