#ifndef __STOUT_NONE_HPP__
#define __STOUT_NONE_HPP__

#include "option.hpp"
#include "result.hpp"

// A "none" type that is implicitly convertible to an Option<T> and
// Result<T> for any T (effectively "syntactic sugar" to make code
// more readable). The implementation uses cast operators to perform
// the conversions instead of adding constructors to Option/Result
// directly. Performance shouldn't be an issue given that an instance
// of None has no virtual functions and no fields.

class None
{
public:
  template <typename T>
  operator Option<T> () const
  {
    return Option<T>::none();
  }

  // Give the compiler some help for nested Option<T>. For example,
  // enable converting None to a Try<Option<T>>. Note that this will
  // bind to the innermost Option<T>.
  template <template <typename> class S, typename T>
  operator S<Option<T> > () const
  {
    return S<Option<T> >(Option<T>::none());
  }

  template <typename T>
  operator Result<T> () const
  {
    return Result<T>::none();
  }

  // Give the compiler some help for nested Result<T>. For example,
  // enable converting None to a Try<Result<T>>. Note that this will
  // bind to the innermost Result<T>.
  template <template <typename> class S, typename T>
  operator S<Result<T> > () const
  {
    return S<Result<T> >(Result<T>::none());
  }

  // Give the compiler some more help to disambiguate the above cast
  // operators from Result<Option<T>>.
  template <typename T>
  operator Result<Option<T> > () const
  {
    return Result<Option<T> >::none();
  }
};

#endif // __STOUT_NONE_HPP__
