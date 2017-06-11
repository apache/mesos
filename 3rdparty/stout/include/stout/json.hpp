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

#ifndef __STOUT_JSON__
#define __STOUT_JSON__

// NOTE: This undef is necessary because we cannot reliably re-order the
// include statements in all cases.  We define this flag globally since
// PicoJson requires it before importing <inttypes.h>.  However, other
// libraries may import <inttypes.h> before we import <picojson.h>.
// Hence, we undefine the flag here to prevent the redefinition error.
#undef __STDC_FORMAT_MACROS
#include <picojson.h>
#define __STDC_FORMAT_MACROS

#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include <boost/variant.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/jsonify.hpp>
#include <stout/numify.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

// TODO(benh): Replace the use of boost::variant here with our wrapper
// `Variant`.

namespace JSON {

// Implementation of the JavaScript Object Notation (JSON) grammar
// using boost::variant. We explicitly define each "type" of the
// grammar, including 'true' (json::True), 'false' (json::False), and
// 'null' (json::Null), for clarity and also because boost::variant
// "picks" the wrong type when we try and use std::string, long (or
// int), double (or float), and bool all in the same variant (while it
// does work with explicit casts, it seemed bad style to force people
// to put those casts in place). We could have avoided using
// json::String or json::Number and just used std::string and double
// respectively, but we choose to include them for completeness
// (although, this does pay a 2x cost when compiling thanks to all the
// extra template instantiations).

// Note that all of these forward declarations are not necessary
// but it serves to document the set of types which are available.
struct String;
struct Number;
struct Object;
struct Array;
struct True;
struct False;
struct Boolean;
struct Null;
struct Value;


struct String
{
  String() {}
  String(const char* _value) : value(_value) {}
  String(const std::string& _value) : value(_value) {}
  std::string value;
};


// NOTE: Due to how PicoJson parses unsigned integers, a roundtrip from Number
// to JSON and back to Number will result in:
//   - a signed integer, if the value is less than or equal to INT64_MAX;
//   - or a double, if the value is greater than INT64_MAX.
// See: https://github.com/kazuho/picojson/blob/rel/v1.3.0/picojson.h#L777-L781
struct Number
{
  Number() : value(0) {}

  template <typename T>
  Number(
      T _value,
      typename std::enable_if<std::is_floating_point<T>::value, int>::type = 0)
    : type(FLOATING), value(_value) {}

  template <typename T>
  Number(
      T _value,
      typename std::enable_if<
          std::is_integral<T>::value && std::is_signed<T>::value,
          int>::type = 0)
    : type(SIGNED_INTEGER), signed_integer(_value) {}

  template <typename T>
  Number(
      T _value,
      typename std::enable_if<
          std::is_integral<T>::value && std::is_unsigned<T>::value,
          int>::type = 0)
    : type(UNSIGNED_INTEGER), unsigned_integer(_value) {}

  template <typename T>
  T as() const
  {
    switch (type) {
      case FLOATING:
        return static_cast<T>(value);
      case SIGNED_INTEGER:
        return static_cast<T>(signed_integer);
      case UNSIGNED_INTEGER:
        return static_cast<T>(unsigned_integer);

      // NOTE: By not setting a default we leverage the compiler
      // errors when the enumeration is augmented to find all
      // the cases we need to provide.
    }

    UNREACHABLE();
  }

  enum Type
  {
    FLOATING,
    SIGNED_INTEGER,
    UNSIGNED_INTEGER,
  } type;

private:
  friend struct Value;
  friend struct Comparator;
  friend void json(NumberWriter* writer, const Number& number);

  union {
    double value;
    int64_t signed_integer;
    uint64_t unsigned_integer;
  };
};


struct Object
{
  Object() = default;

  Object(std::initializer_list<std::pair<const std::string, Value>> values_)
    : values(values_) {}

  // Returns the JSON value (specified by the type) given a "path"
  // into the structure, for example:
  //
  //   Result<JSON::Array> array = object.find<JSON::Array>("nested.array[0]");
  //
  // Will return 'None' if no field could be found called 'array'
  // within a field called 'nested' of 'object' (where 'nested' must
  // also be a JSON object).
  //
  // For 'null' field values, this will return 'Some(Null())' when
  // looking for a matching type ('Null' or 'Value'). If looking for
  // any other type (e.g. 'String', 'Object', etc), this will return
  // 'None' as if the field is not present at all.
  //
  // Returns an error if a JSON value of the wrong type is found, or
  // an intermediate JSON value is not an object that we can do a
  // recursive find on.
  template <typename T>
  Result<T> find(const std::string& path) const;

  // Returns the JSON value by indexing this object with the key. Unlike
  // find(), the key is not a path into the JSON structure, it is just
  // a JSON object key name.
  //
  // Returns 'None' if there key doesn't exist, or an error if a JSON
  // value of the wrong type is found.
  template <typename T>
  Result<T> at(const std::string& key) const;

  std::map<std::string, Value> values;
};


struct Array
{
  Array() = default;
  Array(std::initializer_list<Value> values_) : values(values_) {}

  std::vector<Value> values;
};


struct Boolean
{
  Boolean() : value(false) {}
  Boolean(bool _value) : value(_value) {}
  bool value;
};


// This is a helper so you can say JSON::True() instead of
// JSON::Boolean(true).
struct True : Boolean
{
  True() : Boolean(true) {}
};


// This is a helper so you can say JSON::False() instead of
// JSON::Boolean(false).
struct False : Boolean
{
  False() : Boolean(false) {}
};


struct Null {};


namespace internal {

// Only Object and Array require recursive_wrapper, not sure
// if there is a reason to wrap the others or not.
// Null needs to be first so that it is the default value.
typedef boost::variant<boost::recursive_wrapper<Null>,
                       boost::recursive_wrapper<String>,
                       boost::recursive_wrapper<Number>,
                       boost::recursive_wrapper<Object>,
                       boost::recursive_wrapper<Array>,
                       boost::recursive_wrapper<Boolean>> Variant;

} // namespace internal {


struct Value : internal::Variant
{
  // Empty constructor gets the variant default.
  Value() {}

  Value(bool value) : internal::Variant(JSON::Boolean(value)) {}

  Value(char* value) : internal::Variant(JSON::String(value)) {}
  Value(const char* value) : internal::Variant(JSON::String(value)) {}

  // Arithmetic types are specifically routed through Number because
  // there would be ambiguity between JSON::Bool and JSON::Number
  // otherwise.
  template <typename T>
  Value(
      const T& value,
      typename std::enable_if<std::is_arithmetic<T>::value, int>::type = 0)
    : internal::Variant(Number(value)) {}

  // Non-arithmetic types are passed to the default constructor of
  // Variant.
  template <typename T>
  Value(
      const T& value,
      typename std::enable_if<!std::is_arithmetic<T>::value, int>::type = 0)
    : internal::Variant(value) {}

  template <typename T>
  bool is() const;

  template <typename T>
  const T& as() const;

  // Returns true if and only if 'other' is contained by 'this'.
  // 'Other' is contained by 'this' if the following conditions are
  // fulfilled:
  // 1. If 'other' is a JSON object, then 'this' is also a JSON
  //    object, all keys of 'other' are also present in 'this' and
  //    the value for each key in 'this' also contain the value for
  //    the same key in 'other', i.e. for all keys 'k' in 'other',
  //    'this[k].contains(other[k])' is true.
  // 2. If 'other' is a JSON array, 'this' is also a JSON array, the
  //    length of both arrays is the same and each element in 'this'
  //    also contains the element in 'other' at the same position,
  //    i.e. it holds that this.length() == other.length() and
  //    for each i, 0 <= i < this.length,
  //    'this[i].contains(other[i])'.
  // 3. For all other types, 'this' is of the same type as 'other' and
  //    'this == other'.
  // NOTE: For a given key 'k', if 'this[k] == null' then
  // 'this.contains(other)' holds if either 'k' is not present in
  // 'other.keys()' or 'other[k] == null'.
  // Similarly, if 'other[k] == null', 'this.contains(other)' only if
  // 'this[k] == null'. This is a consequence of the containment
  // definition.
  bool contains(const Value& other) const;

private:
  friend struct Comparator;

  // A class which follows the visitor pattern and implements the
  // containment rules described in the documentation of 'contains'.
  // See 'bool Value::contains(const Value& other) const'.
  struct ContainmentComparator : public boost::static_visitor<bool>
  {
    explicit ContainmentComparator(const Value& _self)
      : self(_self) {}

    bool operator()(const Object& other) const;
    bool operator()(const Array& other) const;
    bool operator()(const String& other) const;
    bool operator()(const Number& other) const;
    bool operator()(const Boolean& other) const;
    bool operator()(const Null&) const;

  private:
    const Value& self;
  };
};


template <typename T>
bool Value::is() const
{
  const T* t = boost::get<T>(this);
  return t != nullptr;
}


template <>
inline bool Value::is<Value>() const
{
  return true;
}


template <typename T>
const T& Value::as() const
{
  return *CHECK_NOTNULL(boost::get<T>(this));
}


template <>
inline const Value& Value::as<Value>() const
{
  return *this;
}


template <typename T>
Result<T> Object::find(const std::string& path) const
{
  const std::vector<std::string> names = strings::split(path, ".", 2);

  if (names.empty()) {
    return None();
  }

  std::string name = names[0];

  // Determine if we have an array subscript. If so, save it but
  // remove it from the name for doing the lookup.
  Option<size_t> subscript = None();
  size_t index = name.find('[');
  if (index != std::string::npos) {
    // Check for the closing bracket.
    if (name.at(name.length() - 1) != ']') {
      return Error("Malformed array subscript, expecting ']'");
    }

    // Now remove the closing bracket (last character) and everything
    // before and including the opening bracket.
    std::string s = name.substr(index + 1, name.length() - index - 2);

    // Now numify the subscript.
    Try<int> i = numify<int>(s);

    if (i.isError()) {
      return Error("Failed to numify array subscript '" + s + "'");
    } else if (i.get() < 0) {
      return Error("Array subscript '" + s + "' must be >= 0");
    }

    subscript = i.get();

    // And finally remove the array subscript from the name.
    name = name.substr(0, index);
  }

  std::map<std::string, Value>::const_iterator entry = values.find(name);

  if (entry == values.end()) {
    return None();
  }

  Value value = entry->second;

  if (subscript.isSome()) {
    if (value.is<Array>()) {
      Array array = value.as<Array>();
      if (subscript.get() >= array.values.size()) {
        return None();
      }
      value = array.values[subscript.get()];
    } else if (value.is<Null>()) {
      return None();
    } else {
      // TODO(benh): Use a visitor to print out the intermediate type.
      return Error("Intermediate JSON value not an array");
    }
  }

  if (names.size() == 1) {
    if (value.is<T>()) {
      return value.as<T>();
    } else if (value.is<Null>()) {
      return None();
    } else {
      // TODO(benh): Use a visitor to print out the type found.
      return Error("Found JSON value of wrong type");
    }
  }

  if (!value.is<Object>()) {
    // TODO(benh): Use a visitor to print out the intermediate type.
    return Error("Intermediate JSON value not an object");
  }

  return value.as<Object>().find<T>(names[1]);
}


template <typename T>
Result<T> Object::at(const std::string& key) const
{
  if (key.empty()) {
    return None();
  }

  std::map<std::string, Value>::const_iterator entry = values.find(key);

  if (entry == values.end()) {
    return None();
  }

  Value value = entry->second;

  if (!value.is<T>()) {
    // TODO(benh): Use a visitor to print out the type found.
    return Error("Found JSON value of wrong type");
  }

  return value.as<T>();
}


inline bool Value::contains(const Value& other) const
{
  return boost::apply_visitor(Value::ContainmentComparator(*this), other);
}


inline bool Value::ContainmentComparator::operator()(const Object& other) const
{
  if (!self.is<Object>()) {
    return false;
  }

  // The empty set is contained in every set.
  if (other.values.empty()) {
    return true;
  }

  const Object& _self = self.as<Object>();

  // All entries in 'other' should exists in 'self', which implies
  // there should be at most as many entries in other as in self.
  if (other.values.size() > _self.values.size()) {
    return false;
  }

  foreachpair (const std::string& key, const Value& value, other.values) {
    auto _selfIterator = _self.values.find(key);

    if (_selfIterator == _self.values.end()) {
      return false;
    }

    if (!_selfIterator->second.contains(value)) {
      return false;
    }
  }

  return true;
}


inline bool Value::ContainmentComparator::operator()(const String& other) const
{
  if (!self.is<String>()) {
    return false;
  }
  return self.as<String>().value == other.value;
}


inline bool Value::ContainmentComparator::operator()(const Number& other) const
{
  if (!self.is<Number>()) {
    return false;
  }

  // NOTE: For the following switch statements, we do not set a default
  // case in order to leverage the compiler errors when the enumeration
  // is augmented to find all the cases we need to provide.

  // NOTE: Using '==' is unsafe for unsigned-signed integer comparisons.
  // The compiler will automatically cast the signed integer to an
  // unsigned integer.  i.e. If the signed integer was negative, it
  // might be converted to a large positive number.
  // Using the '==' operator *is* safe for double-integer comparisons.

  const Number& number = self.as<Number>();
  switch (number.type) {
    case Number::FLOATING:
      switch (other.type) {
        case Number::FLOATING:
          return number.value == other.value;
        case Number::SIGNED_INTEGER:
          return number.value == other.signed_integer;
        case Number::UNSIGNED_INTEGER:
          return number.value == other.unsigned_integer;
      }

    case Number::SIGNED_INTEGER:
      switch (other.type) {
        case Number::FLOATING:
          return number.signed_integer == other.value;
        case Number::SIGNED_INTEGER:
          return number.signed_integer == other.signed_integer;
        case Number::UNSIGNED_INTEGER:
          // See note above for why this is not a simple '==' expression.
          return number.signed_integer >= 0 &&
            number.as<uint64_t>() == other.unsigned_integer;
      }

    case Number::UNSIGNED_INTEGER:
      switch (other.type) {
        case Number::FLOATING:
          return number.unsigned_integer == other.value;
        case Number::SIGNED_INTEGER:
          // See note above for why this is not a simple '==' expression.
          return other.signed_integer >= 0 &&
            number.unsigned_integer == other.as<uint64_t>();
        case Number::UNSIGNED_INTEGER:
          return number.unsigned_integer == other.unsigned_integer;
      }
  }

  UNREACHABLE();
}


inline bool Value::ContainmentComparator::operator()(const Array& other) const
{
  if (!self.is<Array>()) {
    return false;
  }

  const Array& _self = self.as<Array>();

  if (_self.values.size() != other.values.size()) {
    return false;
  }

  for (unsigned i = 0; i < other.values.size(); ++i) {
    if (!_self.values[i].contains(other.values[i])) {
      return false;
    }
  }

  return true;
}


inline bool Value::ContainmentComparator::operator()(const Boolean& other) const
{
  if (!self.is<Boolean>()) {
    return false;
  }
  return self.as<Boolean>().value == other.value;
}


inline bool Value::ContainmentComparator::operator()(const Null&) const
{
  return self.is<Null>();
}


struct Comparator : boost::static_visitor<bool>
{
  Comparator(const Value& _value)
    : value(_value) {}

  bool operator()(const Object& object) const
  {
    if (value.is<Object>()) {
      return value.as<Object>().values == object.values;
    }
    return false;
  }

  bool operator()(const String& string) const
  {
    if (value.is<String>()) {
      return value.as<String>().value == string.value;
    }
    return false;
  }

  bool operator()(const Number& other) const
  {
    // Delegate to the containment comparator.
    // See Value::ContainmentComparator::operator(Number).
    return Value::ContainmentComparator(value)(other);
  }

  bool operator()(const Array& array) const
  {
    if (value.is<Array>()) {
      return value.as<Array>().values == array.values;
    }
    return false;
  }

  bool operator()(const Boolean& boolean) const
  {
    if (value.is<Boolean>()) {
      return value.as<Boolean>().value == boolean.value;
    }
    return false;
  }

  bool operator()(const Null&) const
  {
    return value.is<Null>();
  }

private:
  const Value& value;
};


inline bool operator==(const Value& lhs, const Value& rhs)
{
  return boost::apply_visitor(Comparator(lhs), rhs);
}


inline bool operator!=(const Value& lhs, const Value& rhs)
{
  return !(lhs == rhs);
}

// The following are implementation of `json` customization points in order to
// use `JSON::*` objects with `jsonify`. This also means that `JSON::*` objects
// can be used within other `json` customization points.
//
// For example, we can use `jsonify` directly like this:
//
//   std::cout << jsonify(JSON::Boolean(true));
//
// or, for a user-defined class like this:
//
//   struct S
//   {
//     std::string name;
//     JSON::Value payload;
//   };
//
//   void json(ObjectWriter* writer, const S& s)
//   {
//     writer->field("name", s.name);
//     writer->field("payload", s.payload);  // Printing out a `JSON::Value`!
//   }
//
//   S s{"mpark", JSON::Boolean(true)};
//   std::cout << jsonify(s);  // prints: {"name":"mpark","payload",true}

inline void json(BooleanWriter* writer, const Boolean& boolean)
{
  json(writer, boolean.value);
}


inline void json(StringWriter* writer, const String& string)
{
  json(writer, string.value);
}


inline void json(NumberWriter* writer, const Number& number)
{
  switch (number.type) {
    case Number::FLOATING:
      json(writer, number.value);
      break;
    case Number::SIGNED_INTEGER:
      json(writer, number.signed_integer);
      break;
    case Number::UNSIGNED_INTEGER:
      json(writer, number.unsigned_integer);
      break;
  }
}


inline void json(ObjectWriter* writer, const Object& object)
{
  json(writer, object.values);
}


inline void json(ArrayWriter* writer, const Array& array)
{
  json(writer, array.values);
}


inline void json(NullWriter*, const Null&)
{
  // Do nothing here since `NullWriter` will always just print `null`.
}


// Since `JSON::Value` is a `boost::variant`, we don't know what type of writer
// is required until we visit it. Therefore, we need an implementation of `json`
// which takes a `WriterProxy&&` directly, and constructs the correct writer
// after visitation.
//
// We'd prefer to implement this function similar to the below:
//
//    void json(WriterProxy&& writer, const Value& value)
//    {
//      struct {
//        void operator()(const Number& value) const {
//          json(std::move(writer), value);
//        }
//        void operator()(const String& value) const {
//          json(std::move(writer), value);
//        }
//        /* ... */
//      } visitor;
//      boost::apply_visitor(visitor, value);
//    }
//
// But, `json` is invoked with `WriterProxy` and something like `JSON::Boolean`.
// The version sketched above would be ambiguous with the
// `void json(BooleanWriter*, const Boolean&)` version because both overloads
// involve a single implicit conversion. The `JSON::Boolean` overload has
// a conversion from `WriterProxy` to `BoolWriter*` and the `JSON::Value`
// overload has a conversion from `JSON::Boolean` to `JSON::Value`. In order to
// prefer the overloads such as the one for `JSON::Boolean`, we disallow the
// implicit conversion to `JSON::Value` by declaring as a template.
//
// TODO(mpark): Properly introduce a notion of deferred choice of writers.
// For example, when trying to print a `variant<int, string>` as the value,
// we could take something like a `Writer<Number, String>` which can be turned
// into either a `NumberWriter*` or `StringWriter*`.
template <
    typename T,
    typename std::enable_if<std::is_same<T, Value>::value, int>::type = 0>
void json(WriterProxy&& writer, const T& value)
{
  struct
  {
    using result_type = void;

    void operator()(const Boolean& value) const
    {
      json(std::move(writer_), value);
    }
    void operator()(const String& value) const
    {
      json(std::move(writer_), value);
    }
    void operator()(const Number& value) const
    {
      json(std::move(writer_), value);
    }
    void operator()(const Object& value) const
    {
      json(std::move(writer_), value);
    }
    void operator()(const Array& value) const
    {
      json(std::move(writer_), value);
    }
    void operator()(const Null& value) const
    {
      json(std::move(writer_), value);
    }

    WriterProxy&& writer_;
  } visitor{std::move(writer)};
  boost::apply_visitor(visitor, value);
}


inline std::ostream& operator<<(std::ostream& stream, const Boolean& boolean)
{
  return stream << jsonify(boolean);
}


inline std::ostream& operator<<(std::ostream& stream, const String& string)
{
  return stream << jsonify(string);
}


inline std::ostream& operator<<(std::ostream& stream, const Number& number)
{
  return stream << jsonify(number);
}


inline std::ostream& operator<<(std::ostream& stream, const Object& object)
{
  return stream << jsonify(object);
}


inline std::ostream& operator<<(std::ostream& stream, const Array& array)
{
  return stream << jsonify(array);
}


inline std::ostream& operator<<(std::ostream& stream, const Null& null)
{
  return stream << jsonify(null);
}

namespace internal {

inline Value convert(const picojson::value& value)
{
  if (value.is<picojson::null>()) {
    return Null();
  } else if (value.is<bool>()) {
    return Boolean(value.get<bool>());
  } else if (value.is<picojson::value::object>()) {
    Object object;
    foreachpair (const std::string& name,
                 const picojson::value& v,
                 value.get<picojson::value::object>()) {
      object.values[name] = convert(v);
    }
    return object;
  } else if (value.is<picojson::value::array>()) {
    Array array;
    foreach (const picojson::value& v, value.get<picojson::value::array>()) {
      array.values.push_back(convert(v));
    }
    return array;
  } else if (value.is<int64_t>()) {
    return Number(value.get<int64_t>());
  } else if (value.is<double>()) {
    return Number(value.get<double>());
  } else if (value.is<std::string>()) {
    return String(value.get<std::string>());
  }
  return Null();
}

} // namespace internal {


inline Try<Value> parse(const std::string& s)
{
  const char* parseBegin = s.c_str();
  picojson::value value;
  std::string error;

  // Because PicoJson supports repeated parsing of multiple objects/arrays in a
  // stream, it will quietly ignore trailing non-whitespace characters. We would
  // rather throw an error, however, so use `last_char` to check for this.
  const char* lastVisibleChar =
    parseBegin + s.find_last_not_of(strings::WHITESPACE);

  // Parse the string, returning a pointer to the character
  // immediately following the last one parsed.
  const char* parseEnd =
    picojson::parse(value, parseBegin, parseBegin + s.size(), &error);

  if (!error.empty()) {
    return Error(error);
  } else if (parseEnd != lastVisibleChar + 1) {
    return Error(
        "Parsed JSON included non-whitespace trailing characters: "
        + s.substr(parseEnd - parseBegin, lastVisibleChar + 1 - parseEnd));
  }

  return internal::convert(value);
}


template <typename T>
Try<T> parse(const std::string& s)
{
  Try<Value> value = parse(s);

  if (value.isError()) {
    return Error(value.error());
  }

  if (!value.get().is<T>()) {
    return Error("Unexpected JSON type parsed");
  }

  return value.get().as<T>();
}


template <>
inline Try<Value> parse(const std::string& s)
{
  return parse(s);
}

} // namespace JSON {

#endif // __STOUT_JSON__
