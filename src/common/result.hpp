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
