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

#ifndef __STOUT_FORMAT_HPP__
#define __STOUT_FORMAT_HPP__

#include <stdarg.h> // For 'va_list', 'va_start', 'va_end'.

// For 'vasprintf'.
#ifdef __WINDOWS__
#include <stout/windows/format.hpp>
#else
#include <stdio.h>
#endif // __WINDOWS__

#include <string>
#include <type_traits> // For 'is_pod'.

#include <stout/error.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>


// The 'strings::format' functions produces strings based on the
// printf family of functions. Except, unlike the printf family of
// functions, the 'strings::format' routines attempt to "stringify"
// any arguments that are not POD types (i.e., "plain old data":
// primitives, pointers, certain structs/classes and unions,
// etc). This enables passing structs/classes to 'strings::format'
// provided there is a definition/specialization of 'ostream::operator
// <<' available for that type. Note that the '%s' format specifier is
// expected for each argument that gets stringified. A specialization
// for std::string is also provided so that std::string::c_str is not
// necessary (but again, '%s' is expected as the format specifier).

namespace strings {
namespace internal {

Try<std::string> format(const std::string& fmt, va_list args);
Try<std::string> format(const std::string fmt, ...);

template <typename T, bool b>
struct stringify;

} // namespace internal {


template <typename... T>
Try<std::string> format(const std::string& s, const T&... t)
{
  return internal::format(
      s,
      internal::stringify<T, !std::is_pod<T>::value>(t).get()...);
}


namespace internal {

inline Try<std::string> format(const std::string& fmt, va_list args)
{
  char* temp;
  if (vasprintf(&temp, fmt.c_str(), args) == -1) {
    // Note that temp is undefined, so we do not need to call free.
    return Error("Failed to format '" + fmt + "' (possibly out of memory)");
  }
  std::string result(temp);
  free(temp);
  return result;
}


// NOTE: 'fmt' cannot be 'const std::string&' because passing an
// argument of reference type as the second argument of 'va_start'
// results in undefined behavior.
// Refer to http://stackoverflow.com/a/222314 for further details.
inline Try<std::string> format(const std::string fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  const Try<std::string> result = format(fmt, args);
  va_end(args);
  return result;
}


template <typename T>
struct stringify<T, false>
{
  stringify(const T& _t) : t(_t) {}
  const T& get() { return t; }
  const T& t;
};


template <typename T>
struct stringify<T, true>
{
  stringify(const T& _t) : s(::stringify(_t)) {}
  const char* get() { return s.c_str(); }

  // NOTE: We need to do the copy here, because the temporary returned by
  // ::stringify() doesn't outlive the get() call inside strings::format().
  // TODO(vinod): Figure out a fix for using const ref here.
  const std::string s;
};


template <>
struct stringify<std::string, true>
{
  stringify(const std::string& _s) : s(_s) {}
  const char* get() { return s.c_str(); }
  const std::string& s;
};

} // namespace internal {
} // namespace strings {

#endif // __STOUT_FORMAT_HPP__
