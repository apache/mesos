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

#ifndef __STOUT_OPTION_HPP__
#define __STOUT_OPTION_HPP__

#include <assert.h>

template <typename T>
class Option
{
public:
  static Option<T> none()
  {
    return Option<T>(NONE);
  }

  static Option<T> some(const T& t)
  {
    return Option<T>(SOME, new T(t));
  }

  Option() : state(NONE), t(NULL) {}

  Option(const T& _t) : state(SOME), t(new T(_t)) {}

  Option(const Option<T>& that)
  {
    state = that.state;
    if (that.t != NULL) {
      t = new T(*that.t);
    } else {
      t = NULL;
    }
  }

  ~Option()
  {
    delete t;
  }

  Option<T>& operator = (const Option<T>& that)
  {
    if (this != &that) {
      state = that.state;
      if (that.t != NULL) {
        t = new T(*that.t);
      } else {
        t = NULL;
      }
    }

    return *this;
  }

  bool operator == (const Option<T>& that) const
  {
    return (state == NONE && that.state == NONE) ||
      (state == SOME && that.state == SOME && *t == *that.t);
  }

  bool operator != (const Option<T>& that) const
  {
    return !operator == (that);
  }

  bool isSome() const { return state == SOME; }
  bool isNone() const { return state == NONE; }

  T get() const { assert(state == SOME); return *t; }

private:
  enum State {
    SOME,
    NONE,
  };

  Option(State _state, T* _t = NULL)
    : state(_state), t(_t) {}

  State state;
  T* t;
};

#endif // __STOUT_OPTION_HPP__
