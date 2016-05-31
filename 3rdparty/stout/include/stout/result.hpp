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

#ifndef __STOUT_RESULT_HPP__
#define __STOUT_RESULT_HPP__

#include <assert.h>

#include <iostream>
#include <string>

#include <stout/abort.hpp>
#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/some.hpp>
#include <stout/try.hpp>

// This class is equivalent to Try<Option<T>> and can represent only
// one of these states at a time:
//   1) A value of T.
//   2) No value of T.
//   3) An error state, with a corresponding error string.
// Calling 'isSome' will return true if it stores a value, in which
// case calling 'get' will return a constant reference to the T
// stored. Calling 'isNone' returns true if no value is stored and
// there is no error. Calling 'isError' will return true if it stores
// an error, in which case calling 'error' will return the error
// string.
template <typename T>
class Result
{
public:
  static Result<T> none()
  {
    return Result<T>(None());
  }

  static Result<T> some(const T& t)
  {
    return Result<T>(t);
  }

  static Result<T> error(const std::string& message)
  {
    return Result<T>(Error(message));
  }

  Result(const T& _t)
    : data(Some(_t)) {}

  Result(T&& _t)
    : data(Some(std::move(_t))) {}

  template <
      typename U,
      typename = typename std::enable_if<
          std::is_constructible<T, const U&>::value>::type>
  Result(const U& u)
    : data(Some(u)) {}

  Result(const Option<T>& option)
    : data(option.isSome() ?
           Try<Option<T>>(Some(option.get())) :
           Try<Option<T>>(None())) {}

  Result(const Try<T>& _t)
    : data(_t.isSome() ?
           Try<Option<T>>(Some(_t.get())) :
           Try<Option<T>>(Error(_t.error()))) {}

  Result(const None& none)
    : data(none) {}

  template <typename U>
  Result(const _Some<U>& some)
    : data(some) {}

  Result(const Error& error)
    : data(error) {}

  Result(const ErrnoError& error)
    : data(error) {}

#ifdef __WINDOWS__
  Result(const WindowsError& error)
    : data(error) {}
#endif // __WINDOWS__

  // We don't need to implement these because we are leveraging
  // Try<Option<T>>.
  Result(const Result<T>& that) = default;
  ~Result() = default;
  Result<T>& operator=(const Result<T>& that) = default;
  Result<T>& operator=(Result<T>&& that) = default;

  // 'isSome', 'isNone', and 'isError' are mutually exclusive. They
  // correspond to the underlying unioned state of the Option and Try.
  bool isSome() const { return data.isSome() && data.get().isSome(); }
  bool isNone() const { return data.isSome() && data.get().isNone(); }
  bool isError() const { return data.isError(); }

  const T& get() const
  {
    if (!isSome()) {
      std::string errorMessage = "Result::get() but state == ";
      if (isError()) {
        errorMessage += "ERROR: " + data.error();
      } else if (isNone()) {
        errorMessage += "NONE";
      }
      ABORT(errorMessage);
    }
    return data.get().get();
  }

  T& get()
  {
    return const_cast<T&>(static_cast<const Result&>(*this).get());
  }

  const T* operator->() const { return &get(); }
  T* operator->() { return &get(); }

  const std::string& error() const { assert(isError()); return data.error(); }

private:
  // We leverage Try<Option<T>> to avoid dynamic allocation of T. This
  // means we can take advantage of all the RAII features of 'Try' and
  // makes the implementation of this class much simpler!
  Try<Option<T>> data;
};

#endif // __STOUT_RESULT_HPP__
