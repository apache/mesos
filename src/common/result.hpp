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

#ifndef __RESULT_HPP__
#define __RESULT_HPP__

#include <assert.h>

#include <string>


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

  Result(const Result<T>& that)
  {
    state = that.state;
    if (that.t != NULL) {
      t = new T(*that.t);
    } else {
      t = NULL;
    }
    message = that.message;
  }

  virtual ~Result()
  {
    if (t != NULL) {
      delete t;
    }
  }

  Result<T>& operator = (const Result<T>& that)
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

  bool isSome() { return state == SOME; }
  bool isNone() { return state == NONE; }
  bool isError() { return state == ERROR; }

  T get() { assert(state == SOME); return *t; }

  std::string error() { return message; }

  enum State {
    SOME,
    NONE,
    ERROR
  };

private:
  Result(State _state, T* _t = NULL, const std::string& _message = "")
    : state(_state), t(_t), message(_message) {}

  State state;
  T* t;
  std::string message;
};

#endif // __RESULT_HPP__
