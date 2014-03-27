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
#ifndef __STOUT_FORMAT_HPP__
#define __STOUT_FORMAT_HPP__

#include <stdarg.h> // For 'va_list', 'va_start', 'va_end'.
#include <stdio.h> // For 'vasprintf'.

#include <string>

#if __cplusplus >= 201103L
#include <type_traits> // For 'is_pod'.
#else // __cplusplus >= 201103L
#include <tr1/type_traits> // For 'is_pod'.
#endif // __cplusplus >= 201103L

#include "error.hpp"
#include "try.hpp"
#include "stringify.hpp"


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


#if __cplusplus >= 201103L
template <typename ...T>
Try<std::string> format(const std::string& s, const T& ...t)
{
  return internal::format(
      s,
      internal::stringify<T, !std::is_pod<T>::value>(t).get()...);
}
#else
template <typename T1>
Try<std::string> format(const std::string& s,
                        const T1& t1)
{
  return internal::format(
      s,
      internal::stringify<T1, !std::tr1::is_pod<T1>::value>(t1).get());
}


template <typename T1,
          typename T2>
Try<std::string> format(const std::string& s,
                        const T1& t1,
                        const T2& t2)
{
  return internal::format(
      s,
      internal::stringify<T1, !std::tr1::is_pod<T1>::value>(t1).get(),
      internal::stringify<T2, !std::tr1::is_pod<T1>::value>(t2).get());
}


template <typename T1,
          typename T2,
          typename T3>
Try<std::string> format(const std::string& s,
                        const T1& t1,
                        const T2& t2,
                        const T3& t3)
{
  return internal::format(
      s,
      internal::stringify<T1, !std::tr1::is_pod<T1>::value>(t1).get(),
      internal::stringify<T2, !std::tr1::is_pod<T2>::value>(t2).get(),
      internal::stringify<T3, !std::tr1::is_pod<T3>::value>(t3).get());
}


template <typename T1,
          typename T2,
          typename T3,
          typename T4>
Try<std::string> format(const std::string& s,
                        const T1& t1,
                        const T2& t2,
                        const T3& t3,
                        const T4& t4)
{
  return internal::format(
      s,
      internal::stringify<T1, !std::tr1::is_pod<T1>::value>(t1).get(),
      internal::stringify<T2, !std::tr1::is_pod<T2>::value>(t2).get(),
      internal::stringify<T3, !std::tr1::is_pod<T3>::value>(t3).get(),
      internal::stringify<T4, !std::tr1::is_pod<T4>::value>(t4).get());
}


template <typename T1,
          typename T2,
          typename T3,
          typename T4,
          typename T5>
Try<std::string> format(const std::string& s,
                        const T1& t1,
                        const T2& t2,
                        const T3& t3,
                        const T4& t4,
                        const T5& t5)
{
  return internal::format(
      s,
      internal::stringify<T1, !std::tr1::is_pod<T1>::value>(t1).get(),
      internal::stringify<T2, !std::tr1::is_pod<T2>::value>(t2).get(),
      internal::stringify<T3, !std::tr1::is_pod<T3>::value>(t3).get(),
      internal::stringify<T4, !std::tr1::is_pod<T4>::value>(t4).get(),
      internal::stringify<T5, !std::tr1::is_pod<T5>::value>(t5).get());
}


template <typename T1,
          typename T2,
          typename T3,
          typename T4,
          typename T5,
          typename T6>
Try<std::string> format(const std::string& s,
                        const T1& t1,
                        const T2& t2,
                        const T3& t3,
                        const T4& t4,
                        const T5& t5,
                        const T6& t6)
{
  return internal::format(
      s,
      internal::stringify<T1, !std::tr1::is_pod<T1>::value>(t1).get(),
      internal::stringify<T2, !std::tr1::is_pod<T2>::value>(t2).get(),
      internal::stringify<T3, !std::tr1::is_pod<T3>::value>(t3).get(),
      internal::stringify<T4, !std::tr1::is_pod<T4>::value>(t4).get(),
      internal::stringify<T5, !std::tr1::is_pod<T5>::value>(t5).get(),
      internal::stringify<T6, !std::tr1::is_pod<T6>::value>(t6).get());
}


template <typename T1,
          typename T2,
          typename T3,
          typename T4,
          typename T5,
          typename T6,
          typename T7>
Try<std::string> format(const std::string& s,
                        const T1& t1,
                        const T2& t2,
                        const T3& t3,
                        const T4& t4,
                        const T5& t5,
                        const T6& t6,
                        const T7& t7)
{
  return internal::format(
      s,
      internal::stringify<T1, !std::tr1::is_pod<T1>::value>(t1).get(),
      internal::stringify<T2, !std::tr1::is_pod<T2>::value>(t2).get(),
      internal::stringify<T3, !std::tr1::is_pod<T3>::value>(t3).get(),
      internal::stringify<T4, !std::tr1::is_pod<T4>::value>(t4).get(),
      internal::stringify<T5, !std::tr1::is_pod<T5>::value>(t5).get(),
      internal::stringify<T6, !std::tr1::is_pod<T6>::value>(t6).get(),
      internal::stringify<T7, !std::tr1::is_pod<T7>::value>(t7).get());
}


template <typename T1,
          typename T2,
          typename T3,
          typename T4,
          typename T5,
          typename T6,
          typename T7,
          typename T8>
Try<std::string> format(const std::string& s,
                        const T1& t1,
                        const T2& t2,
                        const T3& t3,
                        const T4& t4,
                        const T5& t5,
                        const T6& t6,
                        const T7& t7,
                        const T8& t8)
{
  return internal::format(
      s,
      internal::stringify<T1, !std::tr1::is_pod<T1>::value>(t1).get(),
      internal::stringify<T2, !std::tr1::is_pod<T2>::value>(t2).get(),
      internal::stringify<T3, !std::tr1::is_pod<T3>::value>(t3).get(),
      internal::stringify<T4, !std::tr1::is_pod<T4>::value>(t4).get(),
      internal::stringify<T5, !std::tr1::is_pod<T5>::value>(t5).get(),
      internal::stringify<T6, !std::tr1::is_pod<T6>::value>(t6).get(),
      internal::stringify<T7, !std::tr1::is_pod<T7>::value>(t7).get(),
      internal::stringify<T8, !std::tr1::is_pod<T8>::value>(t8).get());
}


template <typename T1,
          typename T2,
          typename T3,
          typename T4,
          typename T5,
          typename T6,
          typename T7,
          typename T8,
          typename T9>
Try<std::string> format(const std::string& s,
                        const T1& t1,
                        const T2& t2,
                        const T3& t3,
                        const T4& t4,
                        const T5& t5,
                        const T6& t6,
                        const T7& t7,
                        const T8& t8,
                        const T9& t9)
{
  return internal::format(
      s,
      internal::stringify<T1, !std::tr1::is_pod<T1>::value>(t1).get(),
      internal::stringify<T2, !std::tr1::is_pod<T2>::value>(t2).get(),
      internal::stringify<T3, !std::tr1::is_pod<T3>::value>(t3).get(),
      internal::stringify<T4, !std::tr1::is_pod<T4>::value>(t4).get(),
      internal::stringify<T5, !std::tr1::is_pod<T5>::value>(t5).get(),
      internal::stringify<T6, !std::tr1::is_pod<T6>::value>(t6).get(),
      internal::stringify<T7, !std::tr1::is_pod<T7>::value>(t7).get(),
      internal::stringify<T8, !std::tr1::is_pod<T8>::value>(t8).get(),
      internal::stringify<T9, !std::tr1::is_pod<T9>::value>(t9).get());
}


template <typename T1,
          typename T2,
          typename T3,
          typename T4,
          typename T5,
          typename T6,
          typename T7,
          typename T8,
          typename T9,
          typename T10>
Try<std::string> format(const std::string& s,
                        const T1& t1,
                        const T2& t2,
                        const T3& t3,
                        const T4& t4,
                        const T5& t5,
                        const T6& t6,
                        const T7& t7,
                        const T8& t8,
                        const T9& t9,
                        const T10& t10)
{
  return internal::format(
      s,
      internal::stringify<T1, !std::tr1::is_pod<T1>::value>(t1).get(),
      internal::stringify<T2, !std::tr1::is_pod<T2>::value>(t2).get(),
      internal::stringify<T3, !std::tr1::is_pod<T3>::value>(t3).get(),
      internal::stringify<T4, !std::tr1::is_pod<T4>::value>(t4).get(),
      internal::stringify<T5, !std::tr1::is_pod<T5>::value>(t5).get(),
      internal::stringify<T6, !std::tr1::is_pod<T6>::value>(t6).get(),
      internal::stringify<T7, !std::tr1::is_pod<T7>::value>(t7).get(),
      internal::stringify<T8, !std::tr1::is_pod<T8>::value>(t8).get(),
      internal::stringify<T9, !std::tr1::is_pod<T9>::value>(t9).get(),
      internal::stringify<T10, !std::tr1::is_pod<T10>::value>(t10).get());
}

template <typename T1,
          typename T2,
          typename T3,
          typename T4,
          typename T5,
          typename T6,
          typename T7,
          typename T8,
          typename T9,
          typename T10,
          typename T11>
Try<std::string> format(const std::string& s,
                        const T1& t1,
                        const T2& t2,
                        const T3& t3,
                        const T4& t4,
                        const T5& t5,
                        const T6& t6,
                        const T7& t7,
                        const T8& t8,
                        const T9& t9,
                        const T10& t10,
                        const T11& t11)
{
  return internal::format(
      s,
      internal::stringify<T1, !std::tr1::is_pod<T1>::value>(t1).get(),
      internal::stringify<T2, !std::tr1::is_pod<T2>::value>(t2).get(),
      internal::stringify<T3, !std::tr1::is_pod<T3>::value>(t3).get(),
      internal::stringify<T4, !std::tr1::is_pod<T4>::value>(t4).get(),
      internal::stringify<T5, !std::tr1::is_pod<T5>::value>(t5).get(),
      internal::stringify<T6, !std::tr1::is_pod<T6>::value>(t6).get(),
      internal::stringify<T7, !std::tr1::is_pod<T7>::value>(t7).get(),
      internal::stringify<T8, !std::tr1::is_pod<T8>::value>(t8).get(),
      internal::stringify<T9, !std::tr1::is_pod<T9>::value>(t9).get(),
      internal::stringify<T10, !std::tr1::is_pod<T10>::value>(t10).get(),
      internal::stringify<T11, !std::tr1::is_pod<T11>::value>(t11).get());
}


template <typename T1,
          typename T2,
          typename T3,
          typename T4,
          typename T5,
          typename T6,
          typename T7,
          typename T8,
          typename T9,
          typename T10,
          typename T11,
          typename T12>
Try<std::string> format(const std::string& s,
                        const T1& t1,
                        const T2& t2,
                        const T3& t3,
                        const T4& t4,
                        const T5& t5,
                        const T6& t6,
                        const T7& t7,
                        const T8& t8,
                        const T9& t9,
                        const T10& t10,
                        const T11& t11,
                        const T12& t12)
{
  return internal::format(
      s,
      internal::stringify<T1, !std::tr1::is_pod<T1>::value>(t1).get(),
      internal::stringify<T2, !std::tr1::is_pod<T2>::value>(t2).get(),
      internal::stringify<T3, !std::tr1::is_pod<T3>::value>(t3).get(),
      internal::stringify<T4, !std::tr1::is_pod<T4>::value>(t4).get(),
      internal::stringify<T5, !std::tr1::is_pod<T5>::value>(t5).get(),
      internal::stringify<T6, !std::tr1::is_pod<T6>::value>(t6).get(),
      internal::stringify<T7, !std::tr1::is_pod<T7>::value>(t7).get(),
      internal::stringify<T8, !std::tr1::is_pod<T8>::value>(t8).get(),
      internal::stringify<T9, !std::tr1::is_pod<T9>::value>(t9).get(),
      internal::stringify<T10, !std::tr1::is_pod<T10>::value>(t10).get(),
      internal::stringify<T11, !std::tr1::is_pod<T11>::value>(t11).get(),
      internal::stringify<T12, !std::tr1::is_pod<T12>::value>(t12).get());
}
#endif // __cplusplus >= 201103L


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


inline Try<std::string> format(const std::string fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  const Try<std::string>& result = format(fmt, args);
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
