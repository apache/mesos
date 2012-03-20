/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __TRY_HPP__
#define __TRY_HPP__

#include <assert.h>

#include <string>


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

  Try() : state(ERROR), t(NULL), message("Uninitialized Try") {}

  Try(const T& _t) : state(SOME), t(new T(_t)) {}

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

  ~Try()
  {
    delete t;
  }

  Try<T>& operator = (const Try<T>& that)
  {
    if (this != &that) {
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

  T get() const { assert(state == SOME); return *t; }

  std::string error() const { assert(state == ERROR); return message; }

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


template <>
class Try<void>
{
public:
  static Try<void> some()
  {
    return Try<void>(SOME);
  }

  static Try<void> error(const std::string& message)
  {
    return Try<void>(ERROR, message);
  }

  Try(const Try<void>& that)
  {
    state = that.state;
    message = that.message;
  }

  ~Try() {}

  Try<void>& operator = (const Try<void>& that)
  {
    if (this != &that) {
      state = that.state;
      message = that.message;
    }

    return *this;
  }

  bool isSome() const { return state == SOME; }
  bool isError() const { return state == ERROR; }

  std::string error() const { assert(state == ERROR); return message; }

private:
  enum State {
    SOME,
    ERROR
  };

  Try(State _state, const std::string& _message = "")
    : state(_state), message(_message) {}

  State state;
  std::string message;
};

#endif // __TRY_HPP__
