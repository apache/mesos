#ifndef __STOUT_PROTOBUF_HPP__
#define __STOUT_PROTOBUF_HPP__

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include <sys/types.h>

#include <glog/logging.h>

#include <google/protobuf/message.h>

#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <string>

#include <boost/lexical_cast.hpp>

#include "os.hpp"
#include "result.hpp"
#include "try.hpp"

namespace protobuf {

// Write out the given protobuf to the specified file descriptor by
// first writing out the length of the protobuf followed by the
// contents.
inline Try<bool> write(int fd, const google::protobuf::Message& message)
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
    error = error + " (" + __FILE__ + ":" + stringify(__LINE__) + ")";
    return Try<bool>::error(error);
  }

  CHECK(length != 0);
  CHECK(length == sizeof(size)); // TODO(benh): Handle a non-blocking fd?

  return message.SerializeToFileDescriptor(fd);
}


// A wrapper function that wraps the above write() with
// open and closing the file.
inline Try<bool> write(const std::string& path,
                       const google::protobuf::Message& message)
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
    error = error + " (" + __FILE__ + ":" + stringify(__LINE__) + ")";
    return Result<bool>::error(error);
  }

  uint32_t size;
  ssize_t length = ::read(fd, (void*) &size, sizeof(size));

  if (length == 0) {
    return Result<bool>::none();
  } else if (length == -1) {
    // TODO(bmahler): Handle EINTR by retrying.
    // Save the error, reset the file offset, and return the error.
    std::string error = strerror(errno);
    error = error + " (" + __FILE__ + ":" + stringify(__LINE__) + ")";
    lseek(fd, offset, SEEK_SET);
    return Result<bool>::error(error);
  } else if (length != sizeof(size)) {
    // TODO(bmahler): Handle partial reads with a read loop.
    return false;
  }

  // TODO(benh): Use a different format for writing a protobuf to disk
  // so that we don't need to have broken heuristics like this!
  if (size > 10 * 1024 * 1024) { // 10 MB
    // Save the error, reset the file offset, and return the error.
    std::string error = "Size > 10 MB, possible corruption detected";
    error = error + " (" + __FILE__ + ":" + stringify(__LINE__) + ")";
    lseek(fd, offset, SEEK_SET);
    return Result<bool>::error(error);;
  }

  char* temp = new char[size];

  length = ::read(fd, temp, size);

  if (length == 0) {
    delete[] temp;
    return Result<bool>::none();
  } else if (length == -1) {
    // TODO(bmahler): Handle EINTR by retrying.
    // Save the error, reset the file offset, and return the error.
    std::string error = strerror(errno);
    error = error + " (" + __FILE__ + ":" + stringify(__LINE__) + ")";
    lseek(fd, offset, SEEK_SET);
    delete[] temp;
    return Result<bool>::error(error);
  } else if (length != static_cast<ssize_t>(size)) {
    // TODO(bmahler): Handle partial reads with a read loop.
    delete[] temp;
    return false;
  }

  google::protobuf::io::ArrayInputStream stream(temp, length);
  bool parsed = message->ParseFromZeroCopyStream(&stream);

  delete[] temp;

  if (!parsed) {
    // Save the error, reset the file offset, and return the error.
    std::string error = "Failed to parse protobuf";
    error = error + " (" + __FILE__ + ":" + stringify(__LINE__) + ")";
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
  Try<int> fd = os::open(path, O_RDONLY,
                         S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (fd.isError()) {
    return Result<bool>::error("Failed to open file " + path);
  }

  Result<bool> result = read(fd.get(), message);

  // NOTE: We ignore the return value of close(). This is because users calling
  // this function are interested in the return value of read(). Also an
  // unsuccessful close() doesn't affect the read.
  os::close(fd.get());

  return result;
}


} // namespace protobuf {

#endif // __STOUT_PROTOBUF_HPP__
