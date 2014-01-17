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
#ifndef __STOUT_ERROR_HPP__
#define __STOUT_ERROR_HPP__

#include <errno.h>
#include <string.h> // For strerror.

#include <string>

#include "result.hpp"
#include "try.hpp"

// An "error" type that is implicitly convertible to a Try<T> or
// Result<T> for any T (effectively "syntactic sugar" to make code
// more readable). The implementation uses cast operators to perform
// the conversions instead of adding constructors to Try/Result
// directly. One could imagine revisiting that decision for C++11
// because the use of rvalue reference could eliminate some
// unnecessary copies. However, performance is not critical since
// Error should not get called very often in practice (if so, it's
// probably being used for things that aren't really errors or there
// is a more serious problem during execution).

class Error
{
public:
  explicit Error(const std::string& _message) : message(_message) {}

  template <typename T>
  operator Try<T> () const
  {
    return Try<T>::error(message);
  }

  // Give the compiler some help for nested Try<T>. For example,
  // enable converting Error to an Option<Try<T>>. Note that this will
  // bind to the innermost Try<T>.
  template <template <typename> class S, typename T>
  operator S<Try<T> > () const
  {
    return S<Try<T> >(Try<T>::error(message));
  }

  template <typename T>
  operator Result<T> () const
  {
    return Result<T>::error(message);
  }

  // Give the compiler some help for nested Result<T>. For example,
  // enable converting Error to an Option<Result<T>>. Note that this
  // will bind to the innermost Result<T>.
  template <template <typename> class S, typename T>
  operator S<Result<T> > () const
  {
    return S<Result<T> >(Result<T>::error(message));
  }

  const std::string message;
};


class ErrnoError : public Error
{
public:
  ErrnoError()
    : Error(std::string(strerror(errno))) {}

  ErrnoError(const std::string& message)
    : Error(message + ": " + std::string(strerror(errno))) {}
};

#endif // __STOUT_ERROR_HPP__
