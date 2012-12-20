#ifndef __STOUT_JSON__
#define __STOUT_JSON__

#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <string>

#include <boost/variant.hpp>

#include <stout/foreach.hpp>

// TODO(jsirois): Implement parsing that constructs JSON objects.


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

struct String;
struct Number;
struct Object;
struct Array;
struct True;
struct False;
struct Null;


typedef boost::variant<boost::recursive_wrapper<String>,
                       boost::recursive_wrapper<Number>,
                       boost::recursive_wrapper<Object>,
                       boost::recursive_wrapper<Array>,
                       boost::recursive_wrapper<True>,
                       boost::recursive_wrapper<False>,
                       boost::recursive_wrapper<Null> > Value;


struct String
{
  String() {}
  String(const char* _value) : value(_value) {}
  String(const std::string& _value) : value(_value) {}
  std::string value;
};


struct Number
{
  Number() {}
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


struct True {};


struct False {};


struct Null {};


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
    out.precision(10);
    out << number.value;
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

  void operator () (const True&) const
  {
    out << "true";
  }

  void operator () (const False&) const
  {
    out << "false";
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


inline std::ostream& operator<<(std::ostream& out, const JSON::Value& value)
{
  JSON::render(out, value);
  return out;
}

} // namespace JSON {

#endif // __STOUT_JSON__
