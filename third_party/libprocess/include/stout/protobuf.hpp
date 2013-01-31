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
// first writing out the length of the protobuf followed by the contents.
// NOTE: On error, this may have written partial data to the file.
inline Try<Nothing> write(int fd, const google::protobuf::Message& message)
{
  if (!message.IsInitialized()) {
    return Try<Nothing>::error("protocol buffer is not initialized");
  }

  // First write the size of the protobuf.
  uint32_t size = message.ByteSize();
  std::string bytes = std::string((char*) &size, sizeof(size));

  Try<Nothing> result = os::write(fd, bytes);
  if (result.isError()) {
    return Try<Nothing>::error(
        "Failed to write protobuf size: " + result.error());
  }

  // NOTE: It appears that SerializeToFileDescriptor will keep
  // errno intact on failure, but this may change.
  if (!message.SerializeToFileDescriptor(fd)) {
    return Try<Nothing>::error(
        "Failed to SerializeToFileDescriptor, possible strerror: " +
        std::string(strerror(errno)));
  }

  return Nothing();
}


// A wrapper function that wraps the above write with open and closing the file.
inline Try<Nothing> write(
    const std::string& path,
    const google::protobuf::Message& message)
{
  Try<int> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (fd.isError()) {
    return Try<Nothing>::error(
        "Failed to open file " + path + ": " + fd.error());
  }

  Try<Nothing> result = write(fd.get(), message);

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
  CHECK_NOTNULL(message);

  message->Clear();

  // Save the offset so we can re-adjust if something goes wrong.
  off_t offset = lseek(fd, 0, SEEK_CUR);
  if (offset == -1) {
    return Result<bool>::error(
        "Failed to lseek to SEEK_CUR: " + std::string(strerror(errno)));
  }

  uint32_t size;
  Result<std::string> result = os::read(fd, sizeof(size));

  if (result.isNone()) {
    return Result<bool>::none(); // No more protobufs to read.
  } else if (result.isError()) {
    return Result<bool>::error(
        "Failed to read protobuf size: " + result.error());
  }

  // Parse the size from the bytes.
  memcpy((void*) &size, (void*) result.get().data(), sizeof(size));

  // NOTE: Instead of specifically checking for corruption in 'size', we simply
  // try to read 'size' bytes. If we hit EOF early, it is an indication of
  // corruption.
  result = os::read(fd, size);

  if (result.isNone()) {
    // Hit EOF unexpectedly. Restore the offset to before the size read.
    lseek(fd, offset, SEEK_SET);
    return Result<bool>::error(
        "Failed to read protobuf of size " + stringify(size) + " bytes: "
        "hit EOF unexpectedly, possible corruption");
  } else if (result.isError()) {
    // Restore the offset to before the size read.
    lseek(fd, offset, SEEK_SET);
    return Result<bool>::error("Failed to read protobuf: " + result.error());
  }

  // Parse the protobuf from the string.
  google::protobuf::io::ArrayInputStream stream(
      result.get().data(), result.get().size());
  bool parsed = message->ParseFromZeroCopyStream(&stream);

  if (!parsed) {
    // Restore the offset to before the size read.
    lseek(fd, offset, SEEK_SET);
    // NOTE: It appears that ParseFromZeroCopyStream will keep
    // errno intact on failure, but this may change.
    return Result<bool>::error(
        "Failed to ParseFromZeroCopyStream, possible strerror: " +
        std::string(strerror(errno)));
  }

  return true;
}


// A wrapper function that wraps the above read() with
// open and closing the file.
inline Result<bool> read(
    const std::string& path,
    google::protobuf::Message* message)
{
  Try<int> fd = os::open(
      path, O_RDONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

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
