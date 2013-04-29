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

  bool isSome() const { return state == SOME; }
  bool isNone() const { return state == NONE; }

  T get() const { assert(state == SOME); return *t; }

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

#endif // __STOUT_OPTION_HPP__
