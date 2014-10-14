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
#ifndef __STOUT_TRY_HPP__
#define __STOUT_TRY_HPP__

#include <assert.h>

#include <iostream>
#include <string>

#include <stout/abort.hpp>
#include <stout/error.hpp>

template <typename T>
class Try
{
public:
  static Try<T> some(const T& t)
  {
    return Try<T>(SOME, new T(t));
  }

  static Try<T> error(const std::string& message)
  {
    return Try<T>(ERROR, NULL, message);
  }

  Try(const T& _t)
    : state(SOME), t(new T(_t)) {}

  template <typename U>
  Try(const U& u)
    : state(SOME), t(new T(u)) {}

  Try(const Error& error)
    : state(ERROR), t(NULL), message(error.message) {}

  Try(const ErrnoError& error)
    : state(ERROR), t(NULL), message(error.message) {}

  Try(const Try<T>& that)
  {
    state = that.state;
    if (that.t != NULL) {
      t = new T(*that.t);
    } else {
      t = NULL;
    }
    message = that.message;
  }

  // TODO(bmahler): Add move constructor.

  ~Try()
  {
    delete t;
  }

  Try<T>& operator = (const Try<T>& that)
  {
    if (this != &that) {
      delete t;
      state = that.state;
      if (that.t != NULL) {
        t = new T(*that.t);
      } else {
        t = NULL;
      }
      message = that.message;
    }

    return *this;
  }

  bool isSome() const { return state == SOME; }
  bool isError() const { return state == ERROR; }

  const T& get() const
  {
    if (state != SOME) {
      ABORT("Try::get() but state == ERROR: " + message);
    }
    return *t;
  }

  const std::string& error() const { assert(state == ERROR); return message; }

private:
  enum State {
    SOME,
    ERROR
  };

  Try(State _state, T* _t = NULL, const std::string& _message = "")
    : state(_state), t(_t), message(_message) {}

  State state;
  T* t;
  std::string message;
};


#endif // __STOUT_TRY_HPP__
