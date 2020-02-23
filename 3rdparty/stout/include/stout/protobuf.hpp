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

#ifndef __STOUT_PROTOBUF_HPP__
#define __STOUT_PROTOBUF_HPP__

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#include <sys/types.h>

#include <string>
#include <type_traits>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/repeated_field.h>

#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <stout/abort.hpp>
#include <stout/base64.hpp>
#include <stout/error.hpp>
#include <stout/json.hpp>
#include <stout/jsonify.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/representation.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/os/close.hpp>
#include <stout/os/fsync.hpp>
#include <stout/os/int_fd.hpp>
#include <stout/os/lseek.hpp>
#include <stout/os/open.hpp>
#include <stout/os/read.hpp>
#include <stout/os/write.hpp>

#ifdef __WINDOWS__
#include <stout/os/dup.hpp>
#endif // __WINDOWS__

namespace protobuf {

// TODO(bmahler): Re-use stout's 'recordio' facilities here. Note
// that these use a fixed size length header, whereas stout's
// currently uses a base-10 newline delimited header for language
// portability, which makes changing these a bit tricky.

// Write out the given protobuf to the specified file descriptor by
// first writing out the length of the protobuf followed by the
// contents.
// NOTE: On error, this may have written partial data to the file.
inline Try<Nothing> write(int_fd fd, const google::protobuf::Message& message)
{
  if (!message.IsInitialized()) {
    return Error(message.InitializationErrorString() +
                 " is required but not initialized");
  }

  // First write the size of the protobuf.
  uint32_t size = message.ByteSize();
  std::string bytes((char*) &size, sizeof(size));

  Try<Nothing> result = os::write(fd, bytes);
  if (result.isError()) {
    return Error("Failed to write size: " + result.error());
  }

#ifdef __WINDOWS__
  // NOTE: On Windows, we need to explicitly allocate a CRT file
  // descriptor because the Protobuf library requires it. Because
  // users of `protobuf::write` are likely to call `os::close` on the
  // `fd` we were given, we need to duplicate it before allocating the
  // CRT fd. This is because once the CRT fd is allocated, it must be
  // closed with `_close` instead of `os::close`. Since we need to
  // call `_close` here, we duplicate the fd to prevent the users call
  // of `os::close` from closing twice.
  Try<int_fd> dup = os::dup(fd);
  if (dup.isError()) {
    return Error("Failed to duplicate handle: " + dup.error());
  }

  int crt = dup->crt();

  if (!message.SerializeToFileDescriptor(crt)) {
    ::_close(crt);
    return Error("Failed to write/serialize message");
  }
  ::_close(crt);
#else
  if (!message.SerializeToFileDescriptor(fd)) {
    return Error("Failed to write/serialize message");
  }
#endif

  return Nothing();
}

// Write out the given sequence of protobuf messages to the
// specified file descriptor by repeatedly invoking write
// on each of the messages.
// NOTE: On error, this may have written partial data to the file.
template <typename T>
Try<Nothing> write(
    int_fd fd, const google::protobuf::RepeatedPtrField<T>& messages)
{
  foreach (const T& message, messages) {
    Try<Nothing> result = write(fd, message);
    if (result.isError()) {
      return Error(result.error());
    }
  }

  return Nothing();
}


// A wrapper function for the above `write()` with opening and closing the file.
// If `sync` is set to true, an `fsync()` will be called before `close()`.
template <typename T>
Try<Nothing> write(const std::string& path, const T& t, bool sync = false)
{
  Try<int_fd> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (fd.isError()) {
    return Error("Failed to open file '" + path + "': " + fd.error());
  }

  Try<Nothing> write = ::protobuf::write(fd.get(), t);

  if (sync && write.isSome()) {
    // We call `fsync()` before closing the file instead of opening it with the
    // `O_SYNC` flag for better performance. See:
    // http://lkml.iu.edu/hypermail/linux/kernel/0105.3/0353.html
    write = os::fsync(fd.get());
  }

  Try<Nothing> close = os::close(fd.get());

  // We propagate `close` failures if `write` on the file was successful.
  if (write.isSome() && close.isError()) {
    return Error(
        "Failed to close '" + stringify(fd.get()) + "':" + close.error());
  }

  return write;
}


// A wrapper function to append a protobuf message with opening and closing the
// file. If `sync` is set to true, an `fsync()` will be called before `close()`.
inline Try<Nothing> append(
    const std::string& path,
    const google::protobuf::Message& message,
    bool sync = false)
{
  Try<int_fd> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (fd.isError()) {
    return Error("Failed to open file '" + path + "': " + fd.error());
  }

  Try<Nothing> write = protobuf::write(fd.get(), message);

  if (sync && write.isSome()) {
    // We call `fsync()` before closing the file instead of opening it with the
    // `O_SYNC` flag for better performance.
    write = os::fsync(fd.get());
  }

  Try<Nothing> close = os::close(fd.get());

  // We propagate `close` failures if `write` on the file was successful.
  if (write.isSome() && close.isError()) {
    return Error(
        "Failed to close '" + stringify(fd.get()) + "':" + close.error());
  }

  return write;
}


template <typename T>
Try<T> deserialize(const std::string& value)
{
  T t;
  (void) static_cast<google::protobuf::Message*>(&t);

  // Verify that the size of `value` fits into `ArrayInputStream`'s
  // constructor. The maximum size of a proto2 message is 64 MB, so it is
  // unlikely that we will hit this limit, but since an arbitrary string can be
  // passed in, we include this check to be safe.
  CHECK_LE(value.size(), static_cast<size_t>(std::numeric_limits<int>::max()));
  google::protobuf::io::ArrayInputStream stream(
      value.data(),
      static_cast<int>(value.size()));
  if (!t.ParseFromZeroCopyStream(&stream)) {
    return Error("Failed to deserialize " + t.GetDescriptor()->full_name());
  }
  return t;
}


template <typename T>
Try<std::string> serialize(const T& t)
{
  (void) static_cast<const google::protobuf::Message*>(&t);

  std::string value;
  if (!t.SerializeToString(&value)) {
    return Error("Failed to serialize " + t.GetDescriptor()->full_name());
  }
  return value;
}


namespace internal {

// Reads a single message of type T from the file by first reading the
// "size" followed by the contents (as written by 'write' above).
// NOTE: This struct is used by the public 'read' function.
// See comments there for the reason why we need this.
template <typename T>
struct Read
{
  Result<T> operator()(int_fd fd, bool ignorePartial, bool undoFailed)
  {
    off_t offset = 0;

    if (undoFailed) {
      // Save the offset so we can re-adjust if something goes wrong.
      Try<off_t> lseek = os::lseek(fd, offset, SEEK_CUR);
      if (lseek.isError()) {
        return Error(lseek.error());
      }

      offset = lseek.get();
    }

    uint32_t size;
    Result<std::string> result = os::read(fd, sizeof(size));

    if (result.isError()) {
      if (undoFailed) {
        os::lseek(fd, offset, SEEK_SET);
      }
      return Error("Failed to read size: " + result.error());
    } else if (result.isNone()) {
      return None(); // No more protobufs to read.
    } else if (result->size() < sizeof(size)) {
      // Hit EOF unexpectedly.
      if (undoFailed) {
        // Restore the offset to before the size read.
        os::lseek(fd, offset, SEEK_SET);
      }
      if (ignorePartial) {
        return None();
      }
      return Error(
          "Failed to read size: hit EOF unexpectedly, possible corruption");
    }

    // Parse the size from the bytes.
    memcpy((void*)&size, (void*)result->data(), sizeof(size));

    // NOTE: Instead of specifically checking for corruption in 'size',
    // we simply try to read 'size' bytes. If we hit EOF early, it is an
    // indication of corruption.
    result = os::read(fd, size);

    if (result.isError()) {
      if (undoFailed) {
        // Restore the offset to before the size read.
        os::lseek(fd, offset, SEEK_SET);
      }
      return Error("Failed to read message: " + result.error());
    } else if (result.isNone() || result->size() < size) {
      // Hit EOF unexpectedly.
      if (undoFailed) {
        // Restore the offset to before the size read.
        os::lseek(fd, offset, SEEK_SET);
      }
      if (ignorePartial) {
        return None();
      }
      return Error("Failed to read message of size " + stringify(size) +
                   " bytes: hit EOF unexpectedly, possible corruption");
    }

    // Parse the protobuf from the string.
    // NOTE: We need to capture a const reference to the data because it
    // must outlive the creation of ArrayInputStream.
    const std::string& data = result.get();

    // Verify that the size of `data` fits into `ArrayInputStream`'s
    // constructor. The maximum size of a proto2 message is 64 MB, so it is
    // unlikely that we will hit this limit, but since an arbitrary string can
    // be passed in, we include this check to be safe.
    CHECK_LE(data.size(), static_cast<size_t>(std::numeric_limits<int>::max()));
    T message;
    google::protobuf::io::ArrayInputStream stream(
        data.data(),
        static_cast<int>(data.size()));

    if (!message.ParseFromZeroCopyStream(&stream)) {
      if (undoFailed) {
        // Restore the offset to before the size read.
        os::lseek(fd, offset, SEEK_SET);
      }
      return Error("Failed to deserialize message");
    }

    return message;
  }
};


// Partial specialization for RepeatedPtrField<T> to read a sequence
// of protobuf messages from a given fd by repeatedly invoking
// Read<T> until None is reached, which we treat as EOF.
// NOTE: This struct is used by the public 'read' function.
// See comments there for the reason why we need this.
template <typename T>
struct Read<google::protobuf::RepeatedPtrField<T>>
{
  Result<google::protobuf::RepeatedPtrField<T>> operator()(
      int_fd fd, bool ignorePartial, bool undoFailed)
  {
    google::protobuf::RepeatedPtrField<T> result;
    for (;;) {
      Result<T> message = Read<T>()(fd, ignorePartial, undoFailed);
      if (message.isError()) {
        return Error(message.error());
      } else if (message.isNone()) {
        break;
      } else {
        result.Add()->CopyFrom(message.get());
      }
    }
    return result;
  }
};

}  // namespace internal {


// Reads the protobuf message(s) from a given fd based on the format
// written by write() above. We use partial specialization of
//   - internal::Read<T> vs
//   - internal::Read<google::protobuf::RepeatedPtrField<T>>
// in order to determine whether T is a single protobuf message or
// a sequence of messages.
// If 'ignorePartial' is true, None() is returned when we unexpectedly
// hit EOF while reading the protobuf (e.g., partial write).
// If 'undoFailed' is true, failed read attempts will restore the file
// read/write file offset towards the initial callup position.
template <typename T>
Result<T> read(int_fd fd, bool ignorePartial = false, bool undoFailed = false)
{
  return internal::Read<T>()(fd, ignorePartial, undoFailed);
}


// A wrapper function that wraps the above read() with open and
// closing the file.
template <typename T>
Result<T> read(const std::string& path)
{
  Try<int_fd> fd = os::open(
      path,
      O_RDONLY | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (fd.isError()) {
    return Error("Failed to open file '" + path + "': " + fd.error());
  }

  Result<T> result = read<T>(fd.get());

  // NOTE: We ignore the return value of close(). This is because
  // users calling this function are interested in the return value of
  // read(). Also an unsuccessful close() doesn't affect the read.
  os::close(fd.get());

  return result;
}


namespace internal {

// Forward declaration.
Try<Nothing> parse(
    google::protobuf::Message* message,
    const JSON::Object& object);


struct Parser : boost::static_visitor<Try<Nothing>>
{
  Parser(google::protobuf::Message* _message,
         const google::protobuf::FieldDescriptor* _field)
    : message(_message),
      reflection(message->GetReflection()),
      field(_field) {}

  Try<Nothing> operator()(const JSON::Object& object) const
  {
    switch (field->type()) {
      case google::protobuf::FieldDescriptor::TYPE_MESSAGE:
        if (field->is_map()) {
          foreachpair (
              const std::string& name,
              const JSON::Value& value,
              object.values) {
            google::protobuf::Message* entry =
              reflection->AddMessage(message, field);

            // A map is equivalent to:
            //
            //   message MapFieldEntry {
            //     optional key_type key = 1;
            //     optional value_type value = 2;
            //   }
            //
            //   repeated MapFieldEntry map_field = N;
            //
            // See the link below for details:
            // https://developers.google.com/protocol-buffers/docs/proto#maps
            const google::protobuf::FieldDescriptor* key_field =
              entry->GetDescriptor()->FindFieldByNumber(1);

            JSON::Value key(name);

            Try<Nothing> apply =
              boost::apply_visitor(Parser(entry, key_field), key);

            if (apply.isError()) {
              return Error(apply.error());
            }

            const google::protobuf::FieldDescriptor* value_field =
              entry->GetDescriptor()->FindFieldByNumber(2);

            apply = boost::apply_visitor(Parser(entry, value_field), value);
            if (apply.isError()) {
              return Error(apply.error());
            }
          }
        } else if (field->is_repeated()) {
          // TODO(gilbert): We currently push up the nested error
          // messages without wrapping the error message (due to
          // the recursive nature of parse). We should pass along
          // variable information in order to construct a helpful
          // error message, e.g. "Failed to parse field 'a.b.c': ...".
          return parse(reflection->AddMessage(message, field), object);
        } else {
          return parse(reflection->MutableMessage(message, field), object);
        }
        break;
      default:
        return Error("Not expecting a JSON object for field '" +
                     field->name() + "'");
    }
    return Nothing();
  }

  Try<Nothing> operator()(const JSON::String& string) const
  {
    switch (field->type()) {
      case google::protobuf::FieldDescriptor::TYPE_STRING:
        if (field->is_repeated()) {
          reflection->AddString(message, field, string.value);
        } else {
          reflection->SetString(message, field, string.value);
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_BYTES: {
        Try<std::string> decode = base64::decode(string.value);
        if (decode.isError()) {
          return Error("Failed to base64 decode bytes field"
                       " '" + field->name() + "': " + decode.error());
        }

        if (field->is_repeated()) {
          reflection->AddString(message, field, decode.get());
        } else {
          reflection->SetString(message, field, decode.get());
        }
        break;
      }
      case google::protobuf::FieldDescriptor::TYPE_ENUM: {
        const google::protobuf::EnumValueDescriptor* descriptor =
          field->enum_type()->FindValueByName(string.value);

        if (descriptor == nullptr) {
          if (field->is_required()) {
            return Error("Failed to find enum for '" + string.value + "'");
          }

          // Unrecognized enum value will be discarded if this is not a
          // required enum field, which makes the field's `has..` accessor
          // return false and its getter return the first value listed in
          // the enum definition, or the default value if one is specified.
          //
          // This is the deserialization behavior of proto2, see the link
          // below for details:
          // https://developers.google.com/protocol-buffers/docs/proto#updating
          break;
        }

        if (field->is_repeated()) {
          reflection->AddEnum(message, field, descriptor);
        } else {
          reflection->SetEnum(message, field, descriptor);
        }
        break;
      }
      case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
      case google::protobuf::FieldDescriptor::TYPE_FLOAT: {
        if (string.value == "Infinity") {
          return operator()(JSON::Number(
              std::numeric_limits<double>::infinity()));
        } else if (string.value == "-Infinity") {
          return operator()(JSON::Number(
              -std::numeric_limits<double>::infinity()));
        } else {
          Try<JSON::Number> number = JSON::parse<JSON::Number>(string.value);
          if (number.isError()) {
            return Error(
                "Failed to parse '" + string.value + "' as a JSON number "
                "for field '" + field->name() + "': " + number.error());
          }

          return operator()(number.get());
        }
      }
      case google::protobuf::FieldDescriptor::TYPE_INT64:
      case google::protobuf::FieldDescriptor::TYPE_SINT64:
      case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
      case google::protobuf::FieldDescriptor::TYPE_UINT64:
      case google::protobuf::FieldDescriptor::TYPE_FIXED64:
      case google::protobuf::FieldDescriptor::TYPE_INT32:
      case google::protobuf::FieldDescriptor::TYPE_SINT32:
      case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
      case google::protobuf::FieldDescriptor::TYPE_UINT32:
      case google::protobuf::FieldDescriptor::TYPE_FIXED32: {
        Try<JSON::Number> number = JSON::parse<JSON::Number>(string.value);
        if (number.isError()) {
          return Error(
              "Failed to parse '" + string.value + "' as a JSON number "
              "for field '" + field->name() + "': " + number.error());
        }

        return operator()(number.get());
      }
      case google::protobuf::FieldDescriptor::TYPE_BOOL: {
        Try<JSON::Boolean> boolean = JSON::parse<JSON::Boolean>(string.value);
        if (boolean.isError()) {
          return Error(
              "Failed to parse '" + string.value + "' as a JSON boolean "
              "for field '" + field->name() + "': " + boolean.error());
        }

        return operator()(boolean.get());
      }
      default:
        return Error("Not expecting a JSON string for field '" +
                     field->name() + "'");
    }
    return Nothing();
  }

  Try<Nothing> operator()(const JSON::Number& number) const
  {
    switch (field->type()) {
      case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
        if (field->is_repeated()) {
          reflection->AddDouble(message, field, number.as<double>());
        } else {
          reflection->SetDouble(message, field, number.as<double>());
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_FLOAT:
        if (field->is_repeated()) {
          reflection->AddFloat(message, field, number.as<float>());
        } else {
          reflection->SetFloat(message, field, number.as<float>());
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_INT64:
      case google::protobuf::FieldDescriptor::TYPE_SINT64:
      case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
        if (field->is_repeated()) {
          reflection->AddInt64(message, field, number.as<int64_t>());
        } else {
          reflection->SetInt64(message, field, number.as<int64_t>());
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_UINT64:
      case google::protobuf::FieldDescriptor::TYPE_FIXED64:
        if (field->is_repeated()) {
          reflection->AddUInt64(message, field, number.as<uint64_t>());
        } else {
          reflection->SetUInt64(message, field, number.as<uint64_t>());
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_INT32:
      case google::protobuf::FieldDescriptor::TYPE_SINT32:
      case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
        if (field->is_repeated()) {
          reflection->AddInt32(message, field, number.as<int32_t>());
        } else {
          reflection->SetInt32(message, field, number.as<int32_t>());
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_UINT32:
      case google::protobuf::FieldDescriptor::TYPE_FIXED32:
        if (field->is_repeated()) {
          reflection->AddUInt32(message, field, number.as<uint32_t>());
        } else {
          reflection->SetUInt32(message, field, number.as<uint32_t>());
        }
        break;
      default:
        return Error("Not expecting a JSON number for field '" +
                     field->name() + "'");
    }
    return Nothing();
  }

  Try<Nothing> operator()(const JSON::Array& array) const
  {
    if (!field->is_repeated()) {
      return Error("Not expecting a JSON array for field '" +
                   field->name() + "'");
    }

    foreach (const JSON::Value& value, array.values) {
      Try<Nothing> apply =
        boost::apply_visitor(Parser(message, field), value);

      if (apply.isError()) {
        return Error(apply.error());
      }
    }

    return Nothing();
  }

  Try<Nothing> operator()(const JSON::Boolean& boolean) const
  {
    switch (field->type()) {
      case google::protobuf::FieldDescriptor::TYPE_BOOL:
        if (field->is_repeated()) {
          reflection->AddBool(message, field, boolean.value);
        } else {
          reflection->SetBool(message, field, boolean.value);
        }
        break;
      default:
        return Error("Not expecting a JSON boolean for field '" +
                     field->name() + "'");
    }
    return Nothing();
  }

  Try<Nothing> operator()(const JSON::Null&) const
  {
    // We treat 'null' as an unset field. Note that we allow
    // unset required fields here since the top-level parse
    // function is responsible for checking 'IsInitialized'.
    return Nothing();
  }

private:
  google::protobuf::Message* message;
  const google::protobuf::Reflection* reflection;
  const google::protobuf::FieldDescriptor* field;
};


inline Try<Nothing> parse(
    google::protobuf::Message* message,
    const JSON::Object& object)
{
  foreachpair (
      const std::string& name, const JSON::Value& value, object.values) {
    // Look for a field by this name.
    const google::protobuf::FieldDescriptor* field =
      message->GetDescriptor()->FindFieldByName(name);

    if (field != nullptr) {
      Try<Nothing> apply =
        boost::apply_visitor(Parser(message, field), value);

      if (apply.isError()) {
        return Error(apply.error());
      }
    }
  }

  return Nothing();
}


// Parses a single protobuf message of type T from a JSON::Object.
// NOTE: This struct is used by the public parse<T>() function below. See
// comments there for the reason why we opted for this design.
template <typename T>
struct Parse
{
  Try<T> operator()(const JSON::Value& value)
  {
    static_assert(std::is_convertible<T*, google::protobuf::Message*>::value,
                  "T must be a protobuf message");

    const JSON::Object* object = boost::get<JSON::Object>(&value);
    if (object == nullptr) {
      return Error("Expecting a JSON object");
    }

    T message;

    Try<Nothing> parse = internal::parse(&message, *object);
    if (parse.isError()) {
      return Error(parse.error());
    }

    if (!message.IsInitialized()) {
      return Error("Missing required fields: " +
                   message.InitializationErrorString());
    }

    return message;
  }
};


// Partial specialization for RepeatedPtrField<T> to parse a sequence of
// protobuf messages from a JSON::Array by repeatedly invoking Parse<T> to
// facilitate conversions like JSON::Array -> Resources.
// NOTE: This struct is used by the public parse<T>() function below. See
// comments there for the reason why we opted for this design.
template <typename T>
struct Parse<google::protobuf::RepeatedPtrField<T>>
{
  Try<google::protobuf::RepeatedPtrField<T>> operator()(
      const JSON::Value& value)
  {
    static_assert(std::is_convertible<T*, google::protobuf::Message*>::value,
                  "T must be a protobuf message");

    const JSON::Array* array = boost::get<JSON::Array>(&value);
    if (array == nullptr) {
      return Error("Expecting a JSON array");
    }

    google::protobuf::RepeatedPtrField<T> collection;
    collection.Reserve(static_cast<int>(array->values.size()));

    // Parse messages one by one and propagate an error if it happens.
    foreach (const JSON::Value& elem, array->values) {
      Try<T> message = Parse<T>()(elem);
      if (message.isError()) {
        return Error(message.error());
      }

      collection.Add()->CopyFrom(message.get());
    }

    return collection;
  }
};

} // namespace internal {

// A dispatch wrapper which parses protobuf messages(s) from a given JSON value.
// We use partial specialization of
//   - internal::Parse<T> for JSON::Object
//   - internal::Parse<google::protobuf::RepeatedPtrField<T>> for JSON::Array
// to determine whether T is a single message or a sequence of messages.
// We cannot partially specialize function templates and overloaded function
// approach combined with std::enable_if is not that clean, hence we leverage
// partial specialization of class templates.
template <typename T>
Try<T> parse(const JSON::Value& value)
{
  return internal::Parse<T>()(value);
}

} // namespace protobuf {

namespace JSON {

// The representation of generic protobuf => JSON,
// e.g., `jsonify(JSON::Protobuf(message))`.
struct Protobuf : Representation<google::protobuf::Message>
{
  using Representation<google::protobuf::Message>::Representation;
};


// `json` function for protobuf messages. Refer to `jsonify.hpp` for details.
// TODO(mpark): This currently uses the default value for optional fields
// that are not deprecated, but we may want to revisit this decision.
//
// TODO(mzhu): Use protobuf built-in JSON mapping utilities in favor of
// the reflection APIs. See MESOS-9896.
inline void json(ObjectWriter* writer, const Protobuf& protobuf)
{
  using google::protobuf::FieldDescriptor;

  const google::protobuf::Message& message = protobuf;

  const google::protobuf::Descriptor* descriptor = message.GetDescriptor();
  const google::protobuf::Reflection* reflection = message.GetReflection();

  // We first look through all the possible fields to determine both the set
  // fields __and__ the optional fields with a default that are not set.
  // `Reflection::ListFields()` alone will only include set fields and
  // is therefore insufficient.
  int fieldCount = descriptor->field_count();
  std::vector<const FieldDescriptor*> fields;
  fields.reserve(fieldCount);
  for (int i = 0; i < fieldCount; ++i) {
    const FieldDescriptor* field = descriptor->field(i);
    if (field->is_repeated()) { // Repeated or Map.
      if (reflection->FieldSize(message, field) > 0) {
        // Has repeated field with members, output as JSON.
        fields.push_back(field);
      }
    } else if (
        reflection->HasField(message, field) ||
        (field->has_default_value() && !field->options().deprecated())) {
      // Field is set or has default, output as JSON.
      fields.push_back(field);
    }
  }

  foreach (const FieldDescriptor* field, fields) {
    if (field->is_repeated() && !field->is_map()) {
      writer->field(
          field->name(),
          [&field, &reflection, &message](JSON::ArrayWriter* writer) {
            int fieldSize = reflection->FieldSize(message, field);
            for (int i = 0; i < fieldSize; ++i) {
              switch (field->cpp_type()) {
                case FieldDescriptor::CPPTYPE_BOOL:
                  writer->element(
                      reflection->GetRepeatedBool(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_INT32:
                  writer->element(
                      reflection->GetRepeatedInt32(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_INT64:
                  writer->element(
                      reflection->GetRepeatedInt64(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_UINT32:
                  writer->element(
                      reflection->GetRepeatedUInt32(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_UINT64:
                  writer->element(
                      reflection->GetRepeatedUInt64(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_FLOAT:
                  writer->element(
                      reflection->GetRepeatedFloat(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_DOUBLE:
                  writer->element(
                      reflection->GetRepeatedDouble(message, field, i));
                  break;
                case FieldDescriptor::CPPTYPE_MESSAGE:
                  writer->element(Protobuf(
                      reflection->GetRepeatedMessage(message, field, i)));
                  break;
                case FieldDescriptor::CPPTYPE_ENUM:
                  writer->element(
                      reflection->GetRepeatedEnum(message, field, i)->name());
                  break;
                case FieldDescriptor::CPPTYPE_STRING:
                  const std::string& s = reflection->GetRepeatedStringReference(
                      message, field, i, nullptr);
                  if (field->type() == FieldDescriptor::TYPE_BYTES) {
                    writer->element(base64::encode(s));
                  } else {
                    writer->element(s);
                  }
                  break;
              }
            }
          });
    } else { // field->is_map() || !field->is_repeated()
      auto writeField = [&writer](
                            const std::string& fieldName,
                            const google::protobuf::Reflection* reflection,
                            const google::protobuf::Message& message,
                            const FieldDescriptor* field) {
        switch (field->cpp_type()) {
          case FieldDescriptor::CPPTYPE_BOOL:
            writer->field(fieldName, reflection->GetBool(message, field));
            break;
          case FieldDescriptor::CPPTYPE_INT32:
            writer->field(fieldName, reflection->GetInt32(message, field));
            break;
          case FieldDescriptor::CPPTYPE_INT64:
            writer->field(fieldName, reflection->GetInt64(message, field));
            break;
          case FieldDescriptor::CPPTYPE_UINT32:
            writer->field(fieldName, reflection->GetUInt32(message, field));
            break;
          case FieldDescriptor::CPPTYPE_UINT64:
            writer->field(fieldName, reflection->GetUInt64(message, field));
            break;
          case FieldDescriptor::CPPTYPE_FLOAT:
            writer->field(fieldName, reflection->GetFloat(message, field));
            break;
          case FieldDescriptor::CPPTYPE_DOUBLE:
            writer->field(fieldName, reflection->GetDouble(message, field));
            break;
          case FieldDescriptor::CPPTYPE_MESSAGE:
            writer->field(
                fieldName, Protobuf(reflection->GetMessage(message, field)));
            break;
          case FieldDescriptor::CPPTYPE_ENUM:
            writer->field(
                fieldName, reflection->GetEnum(message, field)->name());
            break;
          case FieldDescriptor::CPPTYPE_STRING:
            const std::string& s =
              reflection->GetStringReference(message, field, nullptr);
            if (field->type() == FieldDescriptor::TYPE_BYTES) {
              writer->field(fieldName, base64::encode(s));
            } else {
              writer->field(fieldName, s);
            }
            break;
        }
      };

      if (!field->is_repeated()) { // Singular field.
        writeField(field->name(), reflection, message, field);
      } else { // Map field.
        CHECK(field->is_map());
        writer->field(
            field->name(),
            [&field, &reflection, &message, &writeField](
                JSON::ObjectWriter* writer) {
              foreach (
                  const auto& mapEntry,
                  reflection->GetRepeatedFieldRef<google::protobuf::Message>(
                      message, field)) {
                const google::protobuf::Descriptor* mapEntryDescriptor =
                  mapEntry.GetDescriptor();
                const google::protobuf::Reflection* mapEntryReflection =
                  mapEntry.GetReflection();

                // Map entry must contain exactly two fields: `key` and `value`.
                CHECK_EQ(mapEntryDescriptor->field_count(), 2);

                const FieldDescriptor* keyField = mapEntryDescriptor->field(0);
                const FieldDescriptor* valueField =
                  mapEntryDescriptor->field(1);

                switch (keyField->cpp_type()) {
                  case FieldDescriptor::CPPTYPE_BOOL:
                    writeField(
                        jsonify(
                            mapEntryReflection->GetBool(mapEntry, keyField)),
                        mapEntryReflection,
                        mapEntry,
                        valueField);
                    break;
                  case FieldDescriptor::CPPTYPE_INT32:
                    writeField(
                        jsonify(
                            mapEntryReflection->GetInt32(mapEntry, keyField)),
                        mapEntryReflection,
                        mapEntry,
                        valueField);
                    break;
                  case FieldDescriptor::CPPTYPE_INT64:
                    writeField(
                        jsonify(
                            mapEntryReflection->GetInt64(mapEntry, keyField)),
                        mapEntryReflection,
                        mapEntry,
                        valueField);
                    break;
                  case FieldDescriptor::CPPTYPE_UINT32:
                    writeField(
                        jsonify(
                            mapEntryReflection->GetUInt32(mapEntry, keyField)),
                        mapEntryReflection,
                        mapEntry,
                        valueField);
                    break;
                  case FieldDescriptor::CPPTYPE_UINT64:
                    writeField(
                        jsonify(
                            mapEntryReflection->GetUInt64(mapEntry, keyField)),
                        mapEntryReflection,
                        mapEntry,
                        valueField);
                    break;
                  case FieldDescriptor::CPPTYPE_STRING:
                    if (keyField->type() == FieldDescriptor::TYPE_BYTES) {
                      LOG(FATAL)
                        << "Unexpected key field type in protobuf Map: "
                        << keyField->type_name();
                    }

                    writeField(
                        mapEntryReflection->GetStringReference(
                            mapEntry, keyField, nullptr),
                        mapEntryReflection,
                        mapEntry,
                        valueField);
                    break;
                  case FieldDescriptor::CPPTYPE_FLOAT:
                  case FieldDescriptor::CPPTYPE_DOUBLE:
                  case FieldDescriptor::CPPTYPE_MESSAGE:
                  case FieldDescriptor::CPPTYPE_ENUM:
                    LOG(FATAL) << "Unexpected key field type in protobuf Map: "
                               << keyField->cpp_type_name();
                }
              }
            });
      }
    }
  }
}


// TODO(bmahler): This currently uses the default value for optional fields
// that are not deprecated, but we may want to revisit this decision.
inline Object protobuf(const google::protobuf::Message& message)
{
  Object object;

  const google::protobuf::Descriptor* descriptor = message.GetDescriptor();
  const google::protobuf::Reflection* reflection = message.GetReflection();

  auto value_for_field = [](
      const google::protobuf::Message& message,
      const google::protobuf::FieldDescriptor* field) -> JSON::Value {
    const google::protobuf::Reflection* reflection = message.GetReflection();

    switch (field->type()) {
      case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
        return JSON::Number(reflection->GetDouble(message, field));
      case google::protobuf::FieldDescriptor::TYPE_FLOAT:
        return JSON::Number(reflection->GetFloat(message, field));
      case google::protobuf::FieldDescriptor::TYPE_INT64:
      case google::protobuf::FieldDescriptor::TYPE_SINT64:
      case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
        return JSON::Number(reflection->GetInt64(message, field));
      case google::protobuf::FieldDescriptor::TYPE_UINT64:
      case google::protobuf::FieldDescriptor::TYPE_FIXED64:
        return JSON::Number(reflection->GetUInt64(message, field));
      case google::protobuf::FieldDescriptor::TYPE_INT32:
      case google::protobuf::FieldDescriptor::TYPE_SINT32:
      case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
        return JSON::Number(reflection->GetInt32(message, field));
      case google::protobuf::FieldDescriptor::TYPE_UINT32:
      case google::protobuf::FieldDescriptor::TYPE_FIXED32:
        return JSON::Number(reflection->GetUInt32(message, field));
      case google::protobuf::FieldDescriptor::TYPE_BOOL:
        if (reflection->GetBool(message, field)) {
          return JSON::Boolean(true);
        } else {
          return JSON::Boolean(false);
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_STRING:
        return JSON::String(reflection->GetString(message, field));
      case google::protobuf::FieldDescriptor::TYPE_BYTES:
        return JSON::String(
            base64::encode(reflection->GetString(message, field)));
      case google::protobuf::FieldDescriptor::TYPE_MESSAGE:
        return protobuf(reflection->GetMessage(message, field));
      case google::protobuf::FieldDescriptor::TYPE_ENUM:
        return JSON::String(reflection->GetEnum(message, field)->name());
      case google::protobuf::FieldDescriptor::TYPE_GROUP:
        // Deprecated! We abort here instead of using a Try as return value,
        // because we expect this code path to never be taken.
        ABORT("Unhandled protobuf field type: " +
              stringify(field->type()));
    }

    UNREACHABLE();
  };

  // We first look through all the possible fields to determine both
  // the set fields _and_ the optional fields with a default that
  // are not set. Reflection::ListFields() alone will only include
  // set fields and is therefore insufficient.
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  fields.reserve(descriptor->field_count());
  for (int i = 0; i < descriptor->field_count(); i++) {
    const google::protobuf::FieldDescriptor* field = descriptor->field(i);
    if (field->is_repeated()) {
      if (reflection->FieldSize(message, descriptor->field(i)) > 0) {
        // Has repeated field with members, output as JSON.
        fields.push_back(field);
      }
    } else if (
        reflection->HasField(message, field) ||
        (field->has_default_value() && !field->options().deprecated())) {
      // Field is set or has default, output as JSON.
      fields.push_back(field);
    }
  }

  foreach (const google::protobuf::FieldDescriptor* field, fields) {
    if (field->is_map()) {
      JSON::Object map;

      int fieldSize = reflection->FieldSize(message, field);
      for (int i = 0; i < fieldSize; ++i) {
        const google::protobuf::Message& entry =
          reflection->GetRepeatedMessage(message, field, i);

        // A map is equivalent to:
        //
        //   message MapFieldEntry {
        //     optional key_type key = 1;
        //     optional value_type value = 2;
        //   }
        //
        //   repeated MapFieldEntry map_field = N;
        //
        // See the link below for details:
        // https://developers.google.com/protocol-buffers/docs/proto#maps
        const google::protobuf::FieldDescriptor* key_field =
          entry.GetDescriptor()->FindFieldByNumber(1);

        const google::protobuf::FieldDescriptor* value_field =
          entry.GetDescriptor()->FindFieldByNumber(2);

        JSON::Value key = value_for_field(entry, key_field);

        std::string name;
        if (key.is<JSON::String>()) {
          name = key.as<JSON::String>().value;
        } else {
          name = jsonify(key);
        }

        map.values[name] = value_for_field(entry, value_field);
      }
      object.values[field->name()] = map;
    } else if (field->is_repeated()) {
      JSON::Array array;
      int fieldSize = reflection->FieldSize(message, field);
      array.values.reserve(fieldSize);
      for (int i = 0; i < fieldSize; ++i) {
        switch (field->type()) {
          case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
            array.values.push_back(JSON::Number(
                reflection->GetRepeatedDouble(message, field, i)));
            break;
          case google::protobuf::FieldDescriptor::TYPE_FLOAT:
            array.values.push_back(JSON::Number(
                reflection->GetRepeatedFloat(message, field, i)));
            break;
          case google::protobuf::FieldDescriptor::TYPE_INT64:
          case google::protobuf::FieldDescriptor::TYPE_SINT64:
          case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
            array.values.push_back(JSON::Number(
                reflection->GetRepeatedInt64(message, field, i)));
            break;
          case google::protobuf::FieldDescriptor::TYPE_UINT64:
          case google::protobuf::FieldDescriptor::TYPE_FIXED64:
            array.values.push_back(JSON::Number(
                reflection->GetRepeatedUInt64(message, field, i)));
            break;
          case google::protobuf::FieldDescriptor::TYPE_INT32:
          case google::protobuf::FieldDescriptor::TYPE_SINT32:
          case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
            array.values.push_back(JSON::Number(
                reflection->GetRepeatedInt32(message, field, i)));
            break;
          case google::protobuf::FieldDescriptor::TYPE_UINT32:
          case google::protobuf::FieldDescriptor::TYPE_FIXED32:
            array.values.push_back(JSON::Number(
                reflection->GetRepeatedUInt32(message, field, i)));
            break;
          case google::protobuf::FieldDescriptor::TYPE_BOOL:
            if (reflection->GetRepeatedBool(message, field, i)) {
              array.values.push_back(JSON::Boolean(true));
            } else {
              array.values.push_back(JSON::Boolean(false));
            }
            break;
          case google::protobuf::FieldDescriptor::TYPE_STRING:
            array.values.push_back(JSON::String(
                reflection->GetRepeatedString(message, field, i)));
            break;
          case google::protobuf::FieldDescriptor::TYPE_BYTES:
            array.values.push_back(JSON::String(base64::encode(
                reflection->GetRepeatedString(message, field, i))));
            break;
          case google::protobuf::FieldDescriptor::TYPE_MESSAGE:
            array.values.push_back(protobuf(
                reflection->GetRepeatedMessage(message, field, i)));
            break;
          case google::protobuf::FieldDescriptor::TYPE_ENUM:
            array.values.push_back(JSON::String(
                reflection->GetRepeatedEnum(message, field, i)->name()));
            break;
          case google::protobuf::FieldDescriptor::TYPE_GROUP:
            // Deprecated! We abort here instead of using a Try as return value,
            // because we expect this code path to never be taken.
            ABORT("Unhandled protobuf field type: " +
                  stringify(field->type()));
        }
      }
      object.values[field->name()] = array;
    } else {
      object.values[field->name()] = value_for_field(message, field);
    }
  }

  return object;
}


template <typename T>
Array protobuf(const google::protobuf::RepeatedPtrField<T>& repeated)
{
  static_assert(std::is_convertible<T*, google::protobuf::Message*>::value,
                "T must be a protobuf message");

  JSON::Array array;
  array.values.reserve(repeated.size());
  foreach (const T& elem, repeated) {
    array.values.emplace_back(JSON::protobuf(elem));
  }

  return array;
}

} // namespace JSON {

#endif // __STOUT_PROTOBUF_HPP__
