#ifndef __STATE_SERIALIZER_HPP__
#define __STATE_SERIALIZER_HPP__

#include <google/protobuf/message.h>

#include <google/protobuf/io/zero_copy_stream_impl.h> // For ArrayInputStream.

#include <string>

#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace state {

struct StringSerializer
{
  template <typename T>
  static Try<std::string> deserialize(const std::string& value)
  {
    return value;
  }

  template <typename T>
  static Try<std::string> serialize(const std::string& value)
  {
    return value;
  }
};


struct ProtobufSerializer
{
  template <typename T>
  static Try<T> deserialize(const std::string& value)
  {
    T t;
    const google::protobuf::Message* message = &t; // Check T is a protobuf.

    google::protobuf::io::ArrayInputStream stream(value.data(), value.size());
    if (!t.ParseFromZeroCopyStream(&stream)) {
      return Try<T>::error(
          "Failed to deserialize " + t.GetDescriptor()->full_name());
    }
    return t;
  }

  template <typename T>
  static Try<std::string> serialize(const T& t)
  {
    // TODO(benh): Actually store the descriptor so that we can verify
    // type information (and compatibility) when we deserialize.
    std::string value;
    if (!t.SerializeToString(&value)) {
      return Try<std::string>::error(
          "Failed to serialize " + t.GetDescriptor()->full_name());
    }
    return value;
  }
};

} // namespace state {
} // namespace internal {
} // namespace mesos {

#endif // __STATE_SERIALIZER_HPP__
