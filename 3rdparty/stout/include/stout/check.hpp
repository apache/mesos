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

#ifndef __STOUT_CHECK_HPP__
#define __STOUT_CHECK_HPP__

#include <ostream>
#include <sstream>
#include <string>

#include <glog/logging.h>

#include <stout/abort.hpp>
#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/some.hpp>
#include <stout/try.hpp>

// A generic macro to facilitate definitions of CHECK_*, akin to CHECK.
// This appends the error if possible to the end of the log message,
// so there's no need to append the error message explicitly.
// To define a new CHECK_*, provide the name, the function that performs the
// check, and the expression. See below for examples (e.g. CHECK_SOME).
#define CHECK_STATE(name, check, expression)                             \
  for (const Option<Error> _error = check(expression); _error.isSome();) \
    _CheckFatal(__FILE__,                                                \
                __LINE__,                                                \
                #name,                                                   \
                #expression,                                             \
                _error.get()).stream()


#define CHECK_SOME(expression) \
  CHECK_STATE(CHECK_SOME, _check_some, expression)


#define CHECK_NONE(expression) \
  CHECK_STATE(CHECK_NONE, _check_none, expression)


#define CHECK_ERROR(expression) \
  CHECK_STATE(CHECK_ERROR, _check_error, expression)


// A private helper for CHECK_NOTNONE which is similar to the
// CHECK_NOTNULL provided by glog.
template <typename T>
T&& _check_not_none(
    const char* file,
    int line,
    const char* message,
    Option<T>&& t) {
  if (t.isNone()) {
    google::LogMessageFatal(file, line, new std::string(message));
  }
  return std::move(t).get();
}


template <typename T>
T& _check_not_none(
    const char* file,
    int line,
    const char* message,
    Option<T>& t) {
  if (t.isNone()) {
    google::LogMessageFatal(file, line, new std::string(message));
  }
  return t.get();
}


template <typename T>
const T& _check_not_none(
    const char* file,
    int line,
    const char* message,
    const Option<T>& t) {
  if (t.isNone()) {
    google::LogMessageFatal(file, line, new std::string(message));
  }
  return t.get();
}


#define CHECK_NOTNONE(expression) \
  _check_not_none( \
      __FILE__, \
      __LINE__, \
      "'" #expression "' Must be SOME", \
      (expression))


// Private structs/functions used for CHECK_*.

template <typename T>
Option<Error> _check_some(const Option<T>& o)
{
  if (o.isNone()) {
    return Error("is NONE");
  } else {
    CHECK(o.isSome());
    return None();
  }
}


template <typename T>
Option<Error> _check_some(const Try<T>& t)
{
  if (t.isError()) {
    return Error(t.error());
  } else {
    CHECK(t.isSome());
    return None();
  }
}


template <typename T>
Option<Error> _check_some(const Result<T>& r)
{
  if (r.isError()) {
    return Error(r.error());
  } else if (r.isNone()) {
    return Error("is NONE");
  } else {
    CHECK(r.isSome());
    return None();
  }
}


template <typename T>
Option<Error> _check_none(const Option<T>& o)
{
  if (o.isSome()) {
    return Error("is SOME");
  } else {
    CHECK(o.isNone());
    return None();
  }
}


template <typename T>
Option<Error> _check_none(const Result<T>& r)
{
  if (r.isError()) {
    return Error("is ERROR");
  } else if (r.isSome()) {
    return Error("is SOME");
  } else {
    CHECK(r.isNone());
    return None();
  }
}


template <typename T>
Option<Error> _check_error(const Try<T>& t)
{
  if (t.isSome()) {
    return Error("is SOME");
  } else {
    CHECK(t.isError());
    return None();
  }
}


template <typename T>
Option<Error> _check_error(const Result<T>& r)
{
  if (r.isNone()) {
    return Error("is NONE");
  } else if (r.isSome()) {
    return Error("is SOME");
  } else {
    CHECK(r.isError());
    return None();
  }
}


struct _CheckFatal
{
  _CheckFatal(const char* _file,
              int _line,
              const char* type,
              const char* expression,
              const Error& error)
      : file(_file),
        line(_line)
  {
    out << type << "(" << expression << "): " << error.message << " ";
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
