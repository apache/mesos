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
#ifndef __STOUT_JSON__
#define __STOUT_JSON__

#include <picojson.h>

#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <boost/type_traits/is_arithmetic.hpp>
#include <boost/utility/enable_if.hpp>
#include <boost/variant.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
#include <stout/numify.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

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


struct Number
{
  Number() : value(0) {}
  Number(double _value) : value(_value) {}
  double value;
};


struct Object
{
  // Returns the JSON value (specified by the type) given a "path"
  // into the structure, for example:
  //
  //   Result<JSON::Array> array = object.find<JSON::Array>("nested.array[0]");
  //
  // Will return 'None' if no field could be found called 'array'
  // within a field called 'nested' of 'object' (where 'nested' must
  // also be a JSON object).
  //
  // Returns an error if a JSON value of the wrong type is found, or
  // an intermediate JSON value is not an object that we can do a
  // recursive find on.
  template <typename T>
  Result<T> find(const std::string& path) const;

  std::map<std::string, Value> values;
};


struct Array
{
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
  True() : Boolean(true) {};
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
                       boost::recursive_wrapper<Boolean> > Variant;

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
      typename boost::enable_if<boost::is_arithmetic<T>, int>::type = 0)
    : internal::Variant(Number(value)) {}

  // Non-arithmetic types are passed to the default constructor of
  // Variant.
  template <typename T>
  Value(
      const T& value,
      typename boost::disable_if<boost::is_arithmetic<T>, int>::type = 0)
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
  // A class which follows the visitor pattern and implements the
  // containment rules described in the documentation of 'contains'.
  // See 'bool Value::contains(const Value& other) const'.
  struct ContainmentComparator : public boost::static_visitor<bool>
  {
    explicit ContainmentComparator(const Value& _self)
      : self(_self) {}

    bool operator () (const Object& other) const;
    bool operator () (const Array& other) const;
    bool operator () (const String& other) const;
    bool operator () (const Number& other) const;
    bool operator () (const Boolean& other) const;
    bool operator () (const Null&) const;

  private:
    const Value& self;
  };
};


template <typename T>
bool Value::is() const
{
  const T* t = boost::get<T>(this);
  return t != NULL;
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

  if (value.is<Array>() && subscript.isSome()) {
    Array array = value.as<Array>();
    if (subscript.get() >= array.values.size()) {
      return None();
    }
    value = array.values[subscript.get()];
  }

  if (names.size() == 1) {
    if (!value.is<T>()) {
      // TODO(benh): Use a visitor to print out the type found.
      return Error("Found JSON value of wrong type");
    }
    return value.as<T>();
  } else if (!value.is<Object>()) {
    // TODO(benh): Use a visitor to print out the intermediate type.
    return Error("Intermediate JSON value not an object");
  }

  return value.as<Object>().find<T>(names[1]);
}


inline bool Value::contains(const Value& other) const
{
  return boost::apply_visitor(Value::ContainmentComparator(*this), other);
}


inline bool Value::ContainmentComparator::operator () (
    const Object& other) const
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


inline bool Value::ContainmentComparator::operator () (
    const String& other) const
{
  if (!self.is<String>()) {
    return false;
  }
  return self.as<String>().value == other.value;
}


inline bool Value::ContainmentComparator::operator () (
    const Number& other) const
{
  if (!self.is<Number>()) {
    return false;
  }
  return self.as<Number>().value == other.value;
}


inline bool Value::ContainmentComparator::operator () (const Array& other) const
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


inline bool Value::ContainmentComparator::operator () (
    const Boolean& other) const
{
  if (!self.is<Boolean>()) {
    return false;
  }
  return self.as<Boolean>().value == other.value;
}


inline bool Value::ContainmentComparator::operator () (const Null&) const
{
  return self.is<Null>();
}


struct Comparator : boost::static_visitor<bool>
{
  Comparator(const Value& _value)
    : value(_value) {}

  bool operator () (const Object& object) const
  {
    if (value.is<Object>()) {
      return value.as<Object>().values == object.values;
    }
    return false;
  }

  bool operator () (const String& string) const
  {
    if (value.is<String>()) {
      return value.as<String>().value == string.value;
    }
    return false;
  }

  bool operator () (const Number& number) const
  {
    if (value.is<Number>()) {
      return value.as<Number>().value == number.value;
    }
    return false;
  }

  bool operator () (const Array& array) const
  {
    if (value.is<Array>()) {
      return value.as<Array>().values == array.values;
    }
    return false;
  }

  bool operator () (const Boolean& boolean) const
  {
    if (value.is<Boolean>()) {
      return value.as<Boolean>().value == boolean.value;
    }
    return false;
  }

  bool operator () (const Null&) const
  {
    return value.is<Null>();
  }

private:
  const Value& value;
};


inline bool operator == (const Value& lhs, const Value& rhs)
{
  return boost::apply_visitor(Comparator(lhs), rhs);
}


inline bool operator != (const Value& lhs, const Value& rhs)
{
  return !(lhs == rhs);
}


inline std::ostream& operator << (std::ostream& out, const String& string)
{
  // TODO(benh): This escaping DOES NOT handle unicode, it encodes as ASCII.
  // See RFC4627 for the JSON string specificiation.
  out << "\"";
  foreach (unsigned char c, string.value) {
    switch (c) {
      case '"':  out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '/':  out << "\\/";  break;
      case '\b': out << "\\b";  break;
      case '\f': out << "\\f";  break;
      case '\n': out << "\\n";  break;
      case '\r': out << "\\r";  break;
      case '\t': out << "\\t";  break;
      default:
        // See RFC4627 for these ranges.
        if ((c >= 0x20 && c <= 0x21) ||
            (c >= 0x23 && c <= 0x5B) ||
            (c >= 0x5D && c < 0x7F)) {
          out << c;
        } else {
          // NOTE: We also escape all bytes > 0x7F since they imply more than
          // 1 byte in UTF-8. This is why we don't escape UTF-8 properly.
          // See RFC4627 for the escaping format: \uXXXX (X is a hex digit).
          // Each byte here will be of the form: \u00XX (this is why we need
          // setw and the cast to unsigned int).
          out << "\\u" << std::setfill('0') << std::setw(4)
              << std::hex << std::uppercase << (unsigned int) c;
        }
        break;
    }
  }
  out << "\"";
  return out;
}


inline std::ostream& operator << (std::ostream& out, const Number& number)
{
  // Use the guaranteed accurate precision, see:
  // http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2006/n2005.pdf
  return out << std::setprecision(std::numeric_limits<double>::digits10)
             << number.value;
}


inline std::ostream& operator << (std::ostream& out, const Object& object)
{
  out << "{";
  std::map<std::string, Value>::const_iterator iterator;
  iterator = object.values.begin();
  while (iterator != object.values.end()) {
    out << String((*iterator).first) << ":" << (*iterator).second;
    if (++iterator != object.values.end()) {
      out << ",";
    }
  }
  out << "}";
  return out;
}


inline std::ostream& operator << (std::ostream& out, const Array& array)
{
  out << "[";
  std::vector<Value>::const_iterator iterator;
  iterator = array.values.begin();
  while (iterator != array.values.end()) {
    out << *iterator;
    if (++iterator != array.values.end()) {
      out << ",";
    }
  }
  out << "]";
  return out;
}


inline std::ostream& operator << (std::ostream& out, const Boolean& boolean)
{
  return out << (boolean.value ? "true" : "false");
}


inline std::ostream& operator << (std::ostream& out, const Null&)
{
  return out << "null";
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
                 const picojson::value& value,
                 value.get<picojson::value::object>()) {
      object.values[name] = convert(value);
    }
    return object;
  } else if (value.is<picojson::value::array>()) {
    Array array;
    foreach (const picojson::value& value,
             value.get<picojson::value::array>()) {
      array.values.push_back(convert(value));
    }
    return array;
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
  picojson::value value;
  std::string error;

  picojson::parse(value, s.c_str(), s.c_str() + s.size(), &error);

  if (!error.empty()) {
    return Error(error);
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
