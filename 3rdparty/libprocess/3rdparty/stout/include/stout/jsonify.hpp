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

#ifndef __STOUT_JSONIFY__
#define __STOUT_JSONIFY__

#include <cstddef>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include <stout/result_of.hpp>
#include <stout/strings.hpp>

// `jsonify` takes an instance of a C++ object and returns a light-weight proxy
// object that can either be implicitly converted to a `std::string`, or
// directly inserted into an output stream.
//
// `jsonify(const T&)` is implemented by calling the function `json`.
// We perform unqualified function call so that it can detect overloads via
// argument dependent lookup. That is, we will search for, and use a free
// function named `json` in the same namespace as `T`.
//
// IMPORTANT: The output stream must not be exception-enabled. This is because
// the writer definitions below insert into the output stream in their
// destructors.
//
// NOTE: This relationship is similar to `boost::hash` and `hash_value`.


// Forward declaration of `JSON::Proxy`.
namespace JSON { class Proxy; }

// Forward declaration of `jsonify`.
template <typename T>
JSON::Proxy jsonify(const T&);

namespace JSON {

// The result of `jsonify`. This is a light-weight proxy object that can either
// be implicitly converted to a `std::string`, or directly inserted into an
// output stream.
//
// In order to make this object light-weight, variables are captured by
// reference. This gives rise to similar semantics as `std::forward_as_tuple`.
// If the arguments are temporaries, `JSON::Proxy` does not extend their
// lifetime; they have to be used before the end of the full expression.
class Proxy
{
public:
  operator std::string() &&
  {
    std::ostringstream stream;
    stream << std::move(*this);
    return stream.str();
  }

private:
  Proxy(std::function<void(std::ostream*)> write) : write_(std::move(write)) {}

  // We declare copy/move constructors `private` to prevent statements that try
  // to "save" an instance of `Proxy` such as:
  //
  //   ```
  //   std::string F();
  //   Proxy proxy = jsonify(F());
  //   ```
  //
  // Since `proxy` in the above example would be holding onto a reference to a
  // temporary string returned by `F()`.
  Proxy(const Proxy&) = default;
  Proxy(Proxy&&) = default;

  std::function<void(std::ostream*)> write_;

  template <typename T>
  friend Proxy (::jsonify)(const T&);

  friend std::ostream& operator<<(std::ostream& stream, Proxy&& that);
};


inline std::ostream& operator<<(std::ostream& stream, Proxy&& that)
{
  that.write_(&stream);
  return stream;
}


// The boolean writer. If `set` is not called at all, `false` is printed.
// If `set` is called more than once, only the last value is printed out.
class BooleanWriter
{
public:
  BooleanWriter(std::ostream* stream) : stream_(stream), value_(false) {}

  BooleanWriter(const BooleanWriter&) = delete;
  BooleanWriter(BooleanWriter&&) = delete;

  ~BooleanWriter() { *stream_ << (value_ ? "true" : "false"); }

  BooleanWriter& operator=(const BooleanWriter&) = delete;
  BooleanWriter& operator=(BooleanWriter&&) = delete;

  void set(bool value) { value_ = value; }

private:
  std::ostream* stream_;
  bool value_;
};


// The number writer. If `set` is not called at all, `0` is printed.
// If `set` is called more than once, only the last value is printed.
class NumberWriter
{
public:
  NumberWriter(std::ostream* stream)
    : stream_(stream), type_(INT), int_(0) {}

  NumberWriter(const NumberWriter&) = delete;
  NumberWriter(NumberWriter&&) = delete;

  ~NumberWriter()
  {
    switch (type_) {
      case INT: {
        *stream_ << int_;
        break;
      }
      case UINT: {
        *stream_ << uint_;
        break;
      }
      case DOUBLE: {
        // Prints a floating point value, with the specified precision, see:
        // http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2006/n2005.pdf
        // Additionally ensures that a decimal point is in the output.
        char buffer[50]; // More than enough for the specified precision.
        const int size = snprintf(
            buffer,
            sizeof(buffer),
            "%#.*g",
            std::numeric_limits<double>::digits10,
            double_);

        // Get rid of excess trailing zeroes before outputting.
        // Otherwise, printing 1.0 would result in "1.00000000000000".
        //
        // NOTE: We intentionally do not use `strings::trim` here in order to
        // avoid construction of temporary strings.
        int back = size - 1;
        for (; back > 0; --back) {
          if (buffer[back] != '0') {
            break;
          }
          buffer[back] = '\0';
        }

        // NOTE: valid JSON numbers cannot end with a '.'.
        *stream_ << buffer << (buffer[back] == '.' ? "0" : "");
        break;
      }
    }
  }

  NumberWriter& operator=(const NumberWriter&) = delete;
  NumberWriter& operator=(NumberWriter&&) = delete;

  // NOTE 1: We enumerate overloads for all of the integral types here to avoid
  // ambiguities between signed and unsigned conversions. If we were to only
  // overload for `long long int` and `unsigned long long int`, passing an
  // argument of `0` would be ambiguous since `0` has type `int`, and cost of
  // conversion to `long long int` or `unsigned long long int` is equivalent.

  // NOTE 2: We use the various modifiers on `int` as opposed to fixed size
  // types such as `int32_t` and `int64_t` because these types do not cover all
  // of the integral types. For example, `uint32_t` may map to `unsigned int`,
  // and `uint64_t` to `unsigned long long int`. If `size_t` maps to `unsigned
  // long int`, it is ambiguous to pass an instance of `size_t`. defining an
  // overload for `size_t` would solve the problem on a specific platform, but
  // we can run into issues again on another platform if `size_t` maps to
  // `unsigned long long int`, since we would get a redefinition error.

  void set(short int value) { set(static_cast<long long int>(value)); }

  void set(int value) { set(static_cast<long long int>(value)); }

  void set(long int value) { set(static_cast<long long int>(value)); }

  void set(long long int value)
  {
    type_ = INT;
    int_ = value;
  }

  void set(unsigned short int value)
  {
    set(static_cast<unsigned long long int>(value));
  }

  void set(unsigned int value)
  {
    set(static_cast<unsigned long long int>(value));
  }

  void set(unsigned long int value)
  {
    set(static_cast<unsigned long long int>(value));
  }

  void set(unsigned long long int value)
  {
    type_ = UINT;
    uint_ = value;
  }

  void set(float value) { set(static_cast<double>(value)); }

  void set(double value)
  {
    type_ = DOUBLE;
    double_ = value;
  }

private:
  std::ostream* stream_;

  enum { INT, UINT, DOUBLE } type_;

  union
  {
    long long int int_;
    unsigned long long int uint_;
    double double_;
  };
};


// The string writer. `append` is used to append a character or a string.
// If `append` is not called at all, `""` is printed.
class StringWriter
{
public:
  StringWriter(std::ostream* stream) : stream_(stream) { *stream_ << '"'; }

  StringWriter(const StringWriter&) = delete;
  StringWriter(StringWriter&&) = delete;

  ~StringWriter() { *stream_ << '"'; }

  StringWriter& operator=(const StringWriter&) = delete;
  StringWriter& operator=(StringWriter&&) = delete;

  void append(char c)
  {
    switch (c) {
      case '"' : *stream_ << "\\\""; break;
      case '\\': *stream_ << "\\\\"; break;
      case '/' : *stream_ <<  "\\/"; break;
      case '\b': *stream_ << "\\b"; break;
      case '\f': *stream_ << "\\f"; break;
      case '\n': *stream_ << "\\n"; break;
      case '\r': *stream_ << "\\r"; break;
      case '\t': *stream_ << "\\t"; break;
      default: {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
          char buffer[7];
          snprintf(buffer, sizeof(buffer), "\\u%04x", c & 0xff);
          stream_->write(buffer, sizeof(buffer) - 1);
        } else {
          *stream_ << c;
        }
        break;
      }
    }
  }

  template <std::size_t N>
  void append(const char (&value)[N]) { append(value, N - 1); }
  void append(const std::string& value) { append(value.data(), value.size()); }

private:
  void append(const char* value, std::size_t size)
  {
    for (std::size_t i = 0; i < size; ++i) {
      append(value[i]);
    }
  }

  std::ostream* stream_;
};


// The array writer. `element(value)` is used to write a new element.
// If `element` is not called at all, `[]` is printed.
class ArrayWriter
{
public:
  ArrayWriter(std::ostream* stream) : stream_(stream), count_(0)
  {
    *stream_ << '[';
  }

  ArrayWriter(const ArrayWriter&) = delete;
  ArrayWriter(ArrayWriter&&) = delete;

  ~ArrayWriter() { *stream_ << ']'; }

  ArrayWriter& operator=(const ArrayWriter&) = delete;
  ArrayWriter& operator=(ArrayWriter&&) = delete;

  template <typename T>
  void element(const T& value)
  {
    if (count_ > 0) {
      *stream_ << ',';
    }
    *stream_ << jsonify(value);
    ++count_;
  }

private:
  std::ostream* stream_;
  std::size_t count_;
};


// The object writer. `field(key, value)` is used to write a new field.
// If `field` is not called at all, `{}` is printed.
class ObjectWriter
{
public:
  ObjectWriter(std::ostream* stream) : stream_(stream), count_(0)
  {
    *stream_ << '{';
  }

  ObjectWriter(const ObjectWriter&) = delete;
  ObjectWriter(ObjectWriter&&) = delete;

  ~ObjectWriter() { *stream_ << '}'; }

  ObjectWriter& operator=(const ObjectWriter&) = delete;
  ObjectWriter& operator=(ObjectWriter&&) = delete;

  template <typename T>
  void field(const std::string& key, const T& value)
  {
    if (count_ > 0) {
      *stream_ << ',';
    }
    *stream_ << jsonify(key) << ':' << jsonify(value);
    ++count_;
  }

private:
  std::ostream* stream_;
  std::size_t count_;
};


// `json` function for boolean.
inline void json(BooleanWriter* writer, bool value) { writer->set(value); }


// `json` functions for numbers.
inline void json(NumberWriter* writer, short int value) { writer->set(value); }
inline void json(NumberWriter* writer, int value) { writer->set(value); }
inline void json(NumberWriter* writer, long int value) { writer->set(value); }


inline void json(NumberWriter* writer, long long int value)
{
  writer->set(value);
}


inline void json(NumberWriter* writer, unsigned short int value)
{
  writer->set(value);
}


inline void json(NumberWriter* writer, unsigned int value)
{
  writer->set(value);
}


inline void json(NumberWriter* writer, unsigned long int value)
{
  writer->set(value);
}


inline void json(NumberWriter* writer, unsigned long long int value)
{
  writer->set(value);
}


inline void json(NumberWriter* writer, float value) { writer->set(value); }
inline void json(NumberWriter* writer, double value) { writer->set(value); }


// `json` functions for strings.

template <std::size_t N>
void json(StringWriter* writer, const char (&value)[N])
{
  writer->append(value);
}


inline void json(StringWriter* writer, const std::string& value)
{
  writer->append(value);
}

namespace internal {

// TODO(mpark): Pull this out to something like <stout/meta.hpp>.
// This pattern already exists in `<process/future.hpp>`.
struct LessPrefer {};
struct Prefer : LessPrefer {};

// The member `value` is `true` if `T` is a sequence, and `false` otherwise.
template <typename T>
struct IsSequence
{
private:
  // This overload only participates in overload resolution if the following
  // expressions are valid.
  //   (1) begin(t) != end(t)
  //   (2) auto iter = begin(t); ++iter
  //   (3) *begin(t)
  //
  // The expressions are only used for SFINAE purposes, and comma operators are
  // used to ignore the results of the expressions. That is, the return type of
  // this function is `decltype(expr0, expr1, expr2, std::true_type{})` which is
  // `std::true_type`.
  template <typename U>
  static auto test(Prefer) -> decltype(
      // Cast to `void` to suppress `-Wunused-comparison` warnings.
      void(std::begin(std::declval<U&>()) != std::end(std::declval<U&>())),
      ++std::declval<decltype(std::begin(std::declval<U&>()))&>(),
      *std::begin(std::declval<U&>()),
      std::true_type{});

  // This overload is chosen if the preferred version is SFINAE'd out.
  template <typename U>
  static std::false_type test(LessPrefer);

public:
  static constexpr bool value = decltype(test<T>(Prefer()))::value;
};


// The member `value` is `true` if `T` has a member typedef `mapped_type`, and
// `false` otherwise. We take the existence of `mapped_type` as the indication
// of an associative container (e.g., std::map).
template <typename T>
struct HasMappedType
{
private:
  template <typename U, typename = typename U::mapped_type>
  static std::true_type test(Prefer);

  template <typename U>
  static std::false_type test(LessPrefer);

public:
  static constexpr bool value = decltype(test<T>(Prefer()))::value;
};

}  // namespace internal {

// `json` function for iterables (e.g., std::vector).
// This function is only enabled if `Iterable` is iterable, is not a
// `const char (&)[N]` (in order to avoid ambiguity with the string literal
// overload), and does not have a member typedef `mapped_type` (we take the
// existence of `mapped_type` as the indication of an associative container).
template <
    typename Iterable,
    typename std::enable_if<
        internal::IsSequence<Iterable>::value &&
        !(std::is_array<Iterable>::value &&
          std::rank<Iterable>::value == 1 &&
          std::is_same<
              char, typename std::remove_extent<Iterable>::type>::value) &&
        !internal::HasMappedType<Iterable>::value, int>::type = 0>
void json(ArrayWriter* writer, const Iterable& iterable)
{
  foreach (const auto& value, iterable) {
    writer->element(value);
  }
}


// `json` function for dictionaries (e.g., std::map).
// This function is only enabled if `Dictionary` is iterable, and has a member
// typedef `mapped_type` (we take the existence of `mapped_type` as the
// indication of an associative container).
template <
    typename Dictionary,
    typename std::enable_if<
        internal::IsSequence<Dictionary>::value &&
        internal::HasMappedType<Dictionary>::value, int>::type = 0>
void json(ObjectWriter* writer, const Dictionary& dictionary)
{
  foreachpair (const auto& key, const auto& value, dictionary) {
    // TODO(mpark): Consider passing `stringify(key)`.
    writer->field(key, value);
  }
}


// An object that can be converted to a pointer to any of the JSON writers.
// This is used to resolve the following scenario:
//
// ```
//   void json(JSON::ObjectWriter*, const Resources&);
//
//   void json(
//       JSON::ArrayWriter*,
//       const google::protobuf::RepeatedPtrField<Resource>&);
//
//   Resources resources;
//   std::cout << jsonify(resources);  // We want to use the first overload!
// ```
//
// The goal is to perform overload resolution based on the second parameter.
// Since `WriterProxy` is convertible to any of the writers equivalently, we
// force overload resolution of `json(WriterProxy(stream), value)` to depend
// only on the second parameter.
class WriterProxy
{
public:
  WriterProxy(std::ostream* stream) : stream_(stream) {}

  ~WriterProxy()
  {
    switch (type_) {
      case BOOLEAN_WRITER: {
        writer_.boolean_writer.~BooleanWriter();
        break;
      }
      case NUMBER_WRITER: {
        writer_.number_writer.~NumberWriter();
        break;
      }
      case STRING_WRITER: {
        writer_.string_writer.~StringWriter();
        break;
      }
      case ARRAY_WRITER: {
        writer_.array_writer.~ArrayWriter();
        break;
      }
      case OBJECT_WRITER: {
        writer_.object_writer.~ObjectWriter();
        break;
      }
    }
  }

  operator BooleanWriter*() &&
  {
    new (&writer_.boolean_writer) BooleanWriter(stream_);
    type_ = BOOLEAN_WRITER;
    return &writer_.boolean_writer;
  }

  operator NumberWriter*() &&
  {
    new (&writer_.number_writer) NumberWriter(stream_);
    type_ = NUMBER_WRITER;
    return &writer_.number_writer;
  }

  operator StringWriter*() &&
  {
    new (&writer_.string_writer) StringWriter(stream_);
    type_ = STRING_WRITER;
    return &writer_.string_writer;
  }

  operator ArrayWriter*() &&
  {
    new (&writer_.array_writer) ArrayWriter(stream_);
    type_ = ARRAY_WRITER;
    return &writer_.array_writer;
  }

  operator ObjectWriter*() &&
  {
    new (&writer_.object_writer) ObjectWriter(stream_);
    type_ = OBJECT_WRITER;
    return &writer_.object_writer;
  }

private:
  enum Type
  {
    BOOLEAN_WRITER,
    NUMBER_WRITER,
    STRING_WRITER,
    ARRAY_WRITER,
    OBJECT_WRITER
  };

  union Writer
  {
    Writer() {}
    ~Writer() {}
    BooleanWriter boolean_writer;
    NumberWriter number_writer;
    StringWriter string_writer;
    ArrayWriter array_writer;
    ObjectWriter object_writer;
  };

  std::ostream* stream_;
  Type type_;
  Writer writer_;
};

namespace internal {

// NOTE: The following overloads of `internal::jsonify` return a `std::function`
// rather than a `JSON::Proxy` since `JSON::Proxy`'s copy/move constructors are
// declared `private`. We could also declare `internal::jsonify` as friend of
// `JSON::Proxy` but chose to minimize friendship and construct a
// `std::function` instead.

// Given an `F` which is a "write" function, we simply use it directly.
template <typename F, typename = typename result_of<F(WriterProxy)>::type>
std::function<void(std::ostream*)> jsonify(const F& write, Prefer)
{
  return [&write](std::ostream* stream) { write(WriterProxy(stream)); };
}

// Given a `T` which is not a "write" function itself, the default "write"
// function is to perform an unqualified function call to `json`, which enables
// argument-dependent lookup. This considers the `json` overloads in the `JSON`
// namespace as well, since `WriterProxy` is intentionally defined in the
// `JSON` namespace.
template <typename T>
std::function<void(std::ostream*)> jsonify(const T& value, LessPrefer)
{
  return [&value](std::ostream* stream) {
    json(WriterProxy(stream), value);
  };
}

} // namespace internal {
} // namespace JSON {

template <typename T>
JSON::Proxy jsonify(const T& t)
{
  return JSON::internal::jsonify(t, JSON::internal::Prefer());
}

#endif // __STOUT_JSONIFY__
