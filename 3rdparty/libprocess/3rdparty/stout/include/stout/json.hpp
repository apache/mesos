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
#include <list>
#include <map>
#include <string>

#include <boost/type_traits/is_arithmetic.hpp>
#include <boost/utility/enable_if.hpp>
#include <boost/variant.hpp>

#include <stout/check.hpp>
#include <stout/foreach.hpp>
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
  std::map<std::string, Value> values;
};


struct Array
{
  std::list<Value> values;
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
  // Empty constructur gets the variant default.
  Value() {}

  // bool creates a JSON::Boolean explicitly.
  Value(bool value) : internal::Variant(JSON::Boolean(value)) {}

  // CStrings create a JSON::String explicitly.
  Value(char* value) : internal::Variant(JSON::String(value)) {}
  Value(const char* value) : internal::Variant(JSON::String(value)) {}

  // Arithmetic types are specifically routed through Number because
  // there would be ambiguity between JSON::Bool and JSON::Number otherwise.
  template <typename T>
  Value(
      const T& value,
      typename boost::enable_if<boost::is_arithmetic<T>, int>::type = 0)
    : internal::Variant(Number(value)) {}

  // Non-arithmetic types are passed to the default constructor of Variant.
  template <typename T>
  Value(
      const T& value,
      typename boost::disable_if<boost::is_arithmetic<T>, int>::type = 0)
    : internal::Variant(value) {}

  template <typename T>
  bool is() const
  {
    const T* t = boost::get<T>(this);
    return t != NULL;
  }

  template <typename T>
  const T& as() const
  {
    return *CHECK_NOTNULL(boost::get<T>(this));
  }
};


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


// Implementation of rendering JSON objects built above using standard
// C++ output streams. The visitor pattern is used thanks to to build
// a "renderer" with boost::static_visitor and two top-level render
// routines are provided for rendering JSON objects and arrays.

struct Renderer : boost::static_visitor<>
{
  Renderer(std::ostream& _out) : out(_out) {}

  void operator () (const String& string) const
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
  }

  void operator () (const Number& number) const
  {
    // Use the guaranteed accurate precision, see:
    // http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2006/n2005.pdf
    out << std::setprecision(std::numeric_limits<double>::digits10)
        << number.value;
  }

  void operator () (const Object& object) const
  {
    out << "{";
    std::map<std::string, Value>::const_iterator iterator;
    iterator = object.values.begin();
    while (iterator != object.values.end()) {
      out << "\"" << (*iterator).first << "\":";
      boost::apply_visitor(Renderer(out), (*iterator).second);
      if (++iterator != object.values.end()) {
        out << ",";
      }
    }
    out << "}";
  }

  void operator () (const Array& array) const
  {
    out << "[";
    std::list<Value>::const_iterator iterator;
    iterator = array.values.begin();
    while (iterator != array.values.end()) {
      boost::apply_visitor(Renderer(out), *iterator);
      if (++iterator != array.values.end()) {
        out << ",";
      }
    }
    out << "]";
  }

  void operator () (const Boolean& boolean) const
  {
    out << (boolean.value ? "true" : "false");
  }

  void operator () (const Null&) const
  {
    out << "null";
  }

private:
  std::ostream& out;
};


inline void render(std::ostream& out, const Value& value)
{
  boost::apply_visitor(Renderer(out), value);
}


inline std::ostream& operator << (std::ostream& out, const Value& value)
{
  render(out, value);
  return out;
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
