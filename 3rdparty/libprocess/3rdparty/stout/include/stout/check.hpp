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
#ifndef __STOUT_CHECK_HPP__
#define __STOUT_CHECK_HPP__

#include <ostream>
#include <sstream>
#include <string>

#include <glog/logging.h>

#include <stout/abort.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/some.hpp>
#include <stout/try.hpp>

// Provides a CHECK_SOME macro, akin to CHECK.
// This appends the error if possible to the end of the log message,
// so there's no need to append the error message explicitly.
#define CHECK_SOME(expression)                                          \
  for (const Option<std::string> _error = _check(expression);           \
       _error.isSome();)                                                \
    _CheckFatal(__FILE__, __LINE__, "CHECK_SOME",                       \
                #expression, _error.get()).stream()

// Private structs/functions used for CHECK_SOME.

template <typename T>
Option<std::string> _check(const Option<T>& o)
{
  if (o.isNone()) {
    return Some("is NONE");
  }
  return None();
}


template <typename T>
Option<std::string> _check(const Try<T>& t)
{
  if (t.isError()) {
    return t.error();
  }
  return None();
}


template <typename T>
Option<std::string> _check(const Result<T>& r)
{
  if (r.isError()) {
    return r.error();
  } else if (r.isNone()) {
    return Some("is NONE");
  }
  return None();
}


struct _CheckFatal
{
  _CheckFatal(const char* _file,
              int _line,
              const char* type,
              const char* expression,
              const std::string& error)
      : file(_file),
        line(_line)
  {
    out << type << "(" << expression << "): " << error << " ";
  }

  ~_CheckFatal()
  {
    google::LogMessageFatal(file.c_str(), line).stream() << out.str();
  }

  std::ostream& stream()
  {
    return out;
  }

  const std::string file;
  const int line;
  std::ostringstream out;
};

#endif // __STOUT_CHECK_HPP__
