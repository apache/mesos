#ifndef __STOUT_FLAGS_PARSE_HPP__
#define __STOUT_FLAGS_PARSE_HPP__

#include <sstream> // For istringstream.
#include <string>

#include <tr1/functional>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/try.hpp>

namespace flags {

template <typename T>
Try<T> parse(const std::string& value)
{
  T t;
  std::istringstream in(value);
  in >> t;
  if (!in.good() && !in.eof()) {
    return Error("Failed to convert into required type");
  }
  return t;
}


template <>
inline Try<std::string> parse(const std::string& value)
{
  return value;
}


template <>
inline Try<bool> parse(const std::string& value)
{
  if (value == "true" || value == "1") {
    return true;
  } else if (value == "false" || value == "0") {
    return false;
  }
  return Error("Expecting a boolean (e.g., true or false)");
}


template <>
inline Try<Duration> parse(const std::string& value)
{
  return Duration::parse(value);
}

} // namespace flags {

#endif // __STOUT_FLAGS_PARSE_HPP__
