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

template <typename T>
class Result
{
public:
  static Result<T> none()
  {
    return Result<T>(NONE);
  }

  static Result<T> some(const T& t)
  {
    return Result<T>(SOME, new T(t));
  }

  static Result<T> error(const std::string& message)
  {
    return Result<T>(ERROR, NULL, message);
  }

  Result(const T& _t)
    : state(SOME), t(new T(_t)) {}

  template <typename U>
  Result(const U& u)
    : state(SOME), t(new T(u)) {}

  Result(const Option<T>& option)
    : state(option.isSome() ? SOME : NONE),
      t(option.isSome() ? new T(option.get()) : NULL) {}

  Result(const None& none)
    : state(NONE), t(NULL) {}

  template <typename U>
  Result(const _Some<U>& some)
    : state(SOME), t(new T(some.t)) {}

  Result(const Error& error)
    : state(ERROR), t(NULL), message(error.message) {}

  Result(const ErrnoError& error)
    : state(ERROR), t(NULL), message(error.message) {}

  Result(const Result<T>& that)
    : state(that.state),
      t(that.t == NULL ? NULL : new T(*that.t)),
      message(that.message) {}

  Result(const Try<T>& _try)
    : state(_try.isSome() ? SOME : ERROR),
      t(_try.isSome() ? new T(_try.get()) : NULL),
      message(_try.isSome() ? "" : _try.error()) {}

  ~Result()
  {
    delete t;
  }

  Result<T>& operator = (const Result<T>& that)
  {
    if (this != &that) {
      delete t;
      state = that.state;
      t = (that.t == NULL ? NULL : new T(*that.t));
      message = that.message;
    }

    return *this;
  }

  bool isSome() const { return state == SOME; }
  bool isNone() const { return state == NONE; }
  bool isError() const { return state == ERROR; }

  const T& get() const
  {
    if (state != SOME) {
      std::string errorMessage = "Result::get() but state == ";
      if (state == ERROR) {
        errorMessage += "ERROR: " + message;
      } else if (state == NONE) {
        errorMessage += "NONE";
      }
      ABORT(errorMessage);
    }
    return *t;
  }

  const std::string& error() const { assert(state == ERROR); return message; }

private:
  enum State {
    SOME,
    NONE,
    ERROR
  };

  Result(State _state, T* _t = NULL, const std::string& _message = "")
    : state(_state), t(_t), message(_message) {}

  State state;
  T* t;
  std::string message;
};

#endif // __STOUT_RESULT_HPP__
