#ifndef __FUTURE_HPP__
#define __FUTURE_HPP__

#include "latch.hpp"


namespace process {

template <typename T>
class Promise;


template <typename T>
class Future
{
public:
  Future();
  Future(const Future<T>& that);
  Future<T>& operator = (const Future<T>& that);
  virtual ~Future();
  T get() const;

private:
  friend class Promise<T>;

  void set(const T& t_);

  int* refs;
  T** t;
  Latch* latch;
};


template <typename T>
Future<T>::Future()
{
  refs = new int;
  *refs = 1;
  t = new T*;
  *t = NULL;
  latch = new Latch();
}


template <typename T>
Future<T>::Future(const Future<T>& that)
{
  assert(that.refs > 0);
  __sync_fetch_and_add(that.refs, 1);
  refs = that.refs;
  t = that.t;
  latch = that.latch;
}


template <typename T>
Future<T>& Future<T>::operator = (const Future<T>& that)
{
  if (this != &that) {
    // Destructor ...
    assert(refs != NULL);
    if (__sync_sub_and_fetch(refs, 1) == 0) {
      delete refs;
      assert(t != NULL);
      if (*t != NULL)
        delete *t;
      assert(latch != NULL);
      delete latch;
    }

    // Copy constructor ...
    assert(that.refs > 0);
    __sync_fetch_and_add(that.refs, 1);
    refs = that.refs;
    t = that.t;
    latch = that.latch;
  }
}


template <typename T>
Future<T>::~Future()
{
  assert(refs != NULL);
  if (__sync_sub_and_fetch(refs, 1) == 0) {
    delete refs;
    assert(t != NULL);
    if (*t != NULL)
      delete *t;
    assert(latch != NULL);
    delete latch;
  }
}


template <typename T>
void Future<T>::set(const T& t_)
{
  assert(t != NULL && *t == NULL);
  *t = new T(t_);
  latch->trigger();
}


template <typename T>
T Future<T>::get() const
{
  assert(t != NULL);
  if (*t != NULL)
    return **t;
  assert(latch != NULL);
  latch->await();
  assert(t != NULL && *t != NULL);
  return **t;
}

}  // namespace process {

#endif // __FUTURE_HPP__
