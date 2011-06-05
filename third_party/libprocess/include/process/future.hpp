#ifndef __FUTURE_HPP__
#define __FUTURE_HPP__

#include <process/latch.hpp>


namespace process {

template <typename T>
class Promise;


template <typename T>
class Future
{
public:
  Future();
  Future(Promise<T>* promise);
  Future(const T& _t);
  Future(const Future<T>& that);
  virtual ~Future();
  Future<T>& operator = (const Future<T>& that);
  bool ready() const;
  bool await(double secs = 0) const;
  T get() const;
  operator T () const;

private:
  friend class Promise<T>;

  void set(const T& _t);

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
Future<T>::Future(Promise<T>* promise)
{
  refs = new int;
  *refs = 1;
  t = new T*;
  *t = NULL;
  latch = new Latch();
  assert(promise != NULL);
  promise->associate(*this);
}


template <typename T>
Future<T>::Future(const T& _t)
{
  refs = new int;
  *refs = 1;
  t = new T*;
  *t = NULL;
  latch = new Latch();
  set(_t);
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
bool Future<T>::ready() const
{
  assert(t != NULL);
  if (*t != NULL)
    return true;
  return false;
}


template <typename T>
bool Future<T>::await(double secs) const
{
  if (ready())
    return true;
  assert(latch != NULL);
  return latch->await(secs);
}


template <typename T>
T Future<T>::get() const
{
  if (ready())
    return **t;
  await();
  assert(t != NULL && *t != NULL);
  return **t;
}


template <typename T>
Future<T>::operator T () const
{
  return get();
}


template <typename T>
void Future<T>::set(const T& _t)
{
  assert(t != NULL && *t == NULL);
  *t = new T(_t);
  latch->trigger();
}

}  // namespace process {

#endif // __FUTURE_HPP__
