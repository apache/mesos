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

#ifndef __STOUT_TRY_HPP__
#define __STOUT_TRY_HPP__

#include <assert.h>

#include <iostream>
#include <string>

#include <stout/abort.hpp>
#include <stout/error.hpp>
#include <stout/option.hpp>
#include <stout/some.hpp>

// This class can represent only one of these states at a time:
//   1) A value of T.
//   2) An error state, with a corresponding error string.
// Calling 'isSome' will return true if it stores a value, in which
// case calling 'get' will return a constant reference to the T
// stored. Calling 'isError' will return true if it stores an error,
// in which case calling 'error' will return the error string.
template <typename T, typename E = Error>
class Try
{
public:
  static_assert(
      std::is_base_of<Error, E>::value,
      "An error type must be, or be inherited from 'Error'.");

  static Try some(const T& t) { return Try(t); }
  static Try error(const E& e) { return Try(e); }

  Try(const T& t)
    : data(Some(t)) {}

  template <
      typename U,
      typename = typename std::enable_if<
          std::is_constructible<T, const U&>::value>::type>
  Try(const U& u) : data(Some(u)) {}

  Try(const E& error) : error_(error) {}

  Try(T&& t)
    : data(Some(std::move(t))) {}

  // We don't need to implement these because we are leveraging
  // Option<T>.
  Try(const Try& that) = default;
  Try(Try&& that) = default;

  ~Try() = default;

  Try& operator=(const Try& that) = default;
  Try& operator=(Try&& that) = default;

  // 'isSome' and 'isError' are mutually exclusive. They correspond
  // to the underlying state of the Option.
  bool isSome() const { return data.isSome(); }
  bool isError() const { return data.isNone(); }

  const T& get() const
  {
    if (!data.isSome()) {
      assert(error_.isSome());
      ABORT("Try::get() but state == ERROR: " + error_.get().message);
    }
    return data.get();
  }

  T& get()
  {
    return const_cast<T&>(static_cast<const Try&>(*this).get());
  }

  const T* operator->() const { return &get(); }
  T* operator->() { return &get(); }

  // NOTE: This function is intended to return the error of type `E`.
  // However, we return a `std::string` if `E` == `Error` since that's what it
  // used to return, and it's the only data that `Error` holds anyway.
  const typename std::conditional<
      std::is_same<E, Error>::value, std::string, E>::type& error() const
  {
    assert(data.isNone());
    assert(error_.isSome());
    return error_impl(error_.get());
  }

private:
  static const std::string& error_impl(const Error& err) { return err.message; }

  template <typename Err>
  static const Err& error_impl(const Err& err) { return err; }

  // We leverage Option<T> to avoid dynamic allocation of T. This
  // means that the storage for T will be included in this object
  // (Try<T>). Since Option<T> keeps track of whether a value is
  // stored, we just ask it when we want to know whether we are
  // storing a value or an error. Another advantage of leveraging
  // Option<T> is that it takes care of all the manual construction
  // and destruction. This makes the code for Try<T> really simple!
  Option<T> data;
  Option<E> error_;
};


#endif // __STOUT_TRY_HPP__
