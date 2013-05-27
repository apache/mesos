#ifndef __STOUT_RESULT_HPP__
#define __STOUT_RESULT_HPP__

#include <assert.h>
#include <stdlib.h> // For abort.

#include <iostream>
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

  Result(const T& _t) : state(SOME), t(new T(_t)) {}

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

  ~Result()
  {
    delete t;
  }

  Result<T>& operator = (const Result<T>& that)
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
  bool isNone() const { return state == NONE; }
  bool isError() const { return state == ERROR; }

  T get() const
  {
    if (state != SOME) {
      if (state == ERROR) {
        std::cerr << "Result::get() but state == ERROR: "
                  << error() << std::endl;
      } else if (state == NONE) {
        std::cerr << "Result::get() but state == NONE" << std::endl;
      }
      abort();
    }
    return *t;
  }

  std::string error() const { assert(state == ERROR); return message; }

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
