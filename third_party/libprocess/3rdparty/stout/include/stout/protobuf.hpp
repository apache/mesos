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

#include "error.hpp"
#include "none.hpp"
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
    return Error("Uninitialized protocol buffer");
  }

  // First write the size of the protobuf.
  uint32_t size = message.ByteSize();
  std::string bytes = std::string((char*) &size, sizeof(size));

  Try<Nothing> result = os::write(fd, bytes);
  if (result.isError()) {
    return Error("Failed to write size: " + result.error());
  }

  if (!message.SerializeToFileDescriptor(fd)) {
    return Error("Failed to write/serialize message");
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
    return Error("Failed to open file '" + path + "': " + fd.error());
  }

  Try<Nothing> result = write(fd.get(), message);

  // NOTE: We ignore the return value of close(). This is because users calling
  // this function are interested in the return value of write(). Also an
  // unsuccessful close() doesn't affect the write.
  os::close(fd.get());

  return result;
}


// Read the next protobuf of type T from the file by first reading the "size"
// followed by the contents (as written by 'write' above).
template <typename T>
inline Result<T> read(int fd)
{
  // Save the offset so we can re-adjust if something goes wrong.
  off_t offset = lseek(fd, 0, SEEK_CUR);
  if (offset == -1) {
    return ErrnoError("Failed to lseek to SEEK_CUR");
  }

  uint32_t size;
  Result<std::string> result = os::read(fd, sizeof(size));

  if (result.isNone()) {
    return None(); // No more protobufs to read.
  } else if (result.isError()) {
    return Error("Failed to read size: " + result.error());
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
    return Error(
        "Failed to read message of size " + stringify(size) + " bytes: "
        "hit EOF unexpectedly, possible corruption");
  } else if (result.isError()) {
    // Restore the offset to before the size read.
    lseek(fd, offset, SEEK_SET);
    return Error("Failed to read message: " + result.error());
  }

  // Parse the protobuf from the string.
  T message;
  google::protobuf::io::ArrayInputStream stream(
      result.get().data(), result.get().size());

  if (!message.ParseFromZeroCopyStream(&stream)) {
    // Restore the offset to before the size read.
    lseek(fd, offset, SEEK_SET);
    return Error("Failed to deserialize message");
  }

  return message;
}


// A wrapper function that wraps the above read() with
// open and closing the file.
template <typename T>
inline Result<T> read(const std::string& path)
{
  Try<int> fd = os::open(
      path, O_RDONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  if (fd.isError()) {
    return Error("Failed to open file '" + path + "': " + fd.error());
  }

  Result<T> result = read<T>(fd.get());

  // NOTE: We ignore the return value of close(). This is because users calling
  // this function are interested in the return value of read(). Also an
  // unsuccessful close() doesn't affect the read.
  os::close(fd.get());

  return result;
}


} // namespace protobuf {

#endif // __STOUT_PROTOBUF_HPP__
