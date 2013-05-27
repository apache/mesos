#ifndef __STOUT_CHECK_HPP__
#define __STOUT_CHECK_HPP__

#include <ostream>
#include <sstream>
#include <string>

#include <glog/logging.h> // Includes LOG(*), PLOG(*), CHECK, etc.

#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

// Provides a CHECK_SOME macro, akin to CHECK.
// This appends the error if possible to the end of the log message, so there's
// no need to append the error message explicitly.
#define CHECK_SOME(expression)                                           \
  for (const Option<std::string>& _error = _check(expression);           \
       _error.isSome();)                                                 \
    _CheckSome(__FILE__, __LINE__, #expression, _error.get()).stream()  \

// Private structs/functions used for CHECK_SOME.

template <typename T>
Option<std::string> _check(const Option<T>& o)
{
  if (o.isNone()) {
    return Option<std::string>::some("is NONE");
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
    return Option<std::string>::some("is NONE");
  }
  return None();
}


struct _CheckSome
{
  _CheckSome(const char* _file,
              int _line,
              const char* _expression,
              const std::string& _error)
    : file(_file),
      line(_line),
      expression(_expression),
      error(_error)
  {
    out << "CHECK_SOME(" << expression << "): ";
  }

  ~_CheckSome()
  {
    out << error;
    google::LogMessageFatal(file.c_str(), line).stream() << out.str();
  }

  std::ostream& stream()
  {
    return out;
  }

  const std::string file;
  const int line;
  const std::string expression;
  const std::string error;
  std::ostringstream out;
};

#endif // __STOUT_CHECK_HPP__
