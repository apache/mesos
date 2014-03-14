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
#ifndef __STOUT_OPTION_HPP__
#define __STOUT_OPTION_HPP__

#include <assert.h>

#include <algorithm>

#include <stout/none.hpp>
#include <stout/some.hpp>

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

  template <typename U>
  Option(const U& u) : state(SOME), t(new T(u)) {}

  Option(const None& none) : state(NONE), t(NULL) {}

  template <typename U>
  Option(const _Some<U>& some) : state(SOME), t(new T(some.t)) {}

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
      delete t;
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

  bool operator == (const T& that) const
  {
    return state == SOME && *t == that;
  }

  bool operator != (const T& that) const
  {
    return !operator == (that);
  }

  bool isSome() const { return state == SOME; }
  bool isNone() const { return state == NONE; }

  const T& get() const { assert(state == SOME); return *t; }

  // This must return a copy to avoid returning a reference to a temporary.
  T get(const T& _t) const { return state == NONE ? _t : *t; }

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


template <typename T>
Option<T> min(const Option<T>& left, const Option<T>& right)
{
  if (left.isSome() && right.isSome()) {
    return std::min(left.get(), right.get());
  } else if (left.isSome()) {
    return left.get();
  } else if (right.isSome()) {
    return right.get();
  } else {
    return Option<T>::none();
  }
}


template <typename T>
Option<T> min(const Option<T>& left, const T& right)
{
  return min(left, Option<T>(right));
}


template <typename T>
Option<T> min(const T& left, const Option<T>& right)
{
  return min(Option<T>(left), right);
}


template <typename T>
Option<T> max(const Option<T>& left, const Option<T>& right)
{
  if (left.isSome() && right.isSome()) {
    return std::max(left.get(), right.get());
  } else if (left.isSome()) {
    return left.get();
  } else if (right.isSome()) {
    return right.get();
  } else {
    return Option<T>::none();
  }
}


template <typename T>
Option<T> max(const Option<T>& left, const T& right)
{
  return max(left, Option<T>(right));
}


template <typename T>
Option<T> max(const T& left, const Option<T>& right)
{
  return max(Option<T>(left), right);
}

#endif // __STOUT_OPTION_HPP__
