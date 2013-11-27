#ifndef __STOUT_SOME_HPP__
#define __STOUT_SOME_HPP__

#include <stout/option.hpp>
#include <stout/result.hpp>

// A "some" type that is implicitely convertable to an Option<T> and
// Result<T> for any T (effectively "syntactic sugar" to make code
// more readable). The implementation uses cast operators to perform
// the conversions instead of adding constructors to Option/Result
// directly. The extra copies involved here can be elided with C++11
// rvalue references. Furthermore, since in most circumstances a Some
// will not be needed (an Option<T> or Result<T> can be constructed
// directly from the value) we don't worry about performance.

template <typename T>
struct _Some
{
  _Some(T _t) : t(_t) {}

  template <typename U>
  operator Option<U> () const
  {
    return Option<U>::some(t);
  }

  // Give the compiler some help for nested Option<U>.
  template <template <typename> class S, typename U>
  operator S<Option<U> > () const
  {
    return S<Option<U> >(Option<U>::some(t));
  }

  template <typename U>
  operator Result<U> () const
  {
    return Result<U>::some(t);
  }

  // Give the compiler some help for nested Result<U>.
  template <template <typename> class S, typename U>
  operator S<Result<U> > () const
  {
    return S<Result<U> >(Result<U>::some(t));
  }

  // Give the compiler some more help to disambiguate the above cast
  // operators from Option<Result<U>>.
  template <typename U>
  operator Option<Result<U> > () const
  {
    return Option<Result<U> >::some(t);
  }

  // Give the compiler some more help to disambiguate the above cast
  // operators from Result<Option<U>>.
  template <typename U>
  operator Result<Option<U> > () const
  {
    return Result<Option<U> >::some(t);
  }

  const T t;
};


template <typename T>
_Some<T> Some(T t)
{
  return _Some<T>(t);
}

#endif // __STOUT_SOME_HPP__
