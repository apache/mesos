#ifndef __OPTION_HPP__
#define __OPTION_HPP__

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
    if (t != NULL) {
      delete t;
    }
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

  bool isSome() { return state == SOME; }
  bool isNone() { return state == NONE; }

  T get() { assert(state == SOME); return *t; }

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

#endif // __OPTION_HPP__

