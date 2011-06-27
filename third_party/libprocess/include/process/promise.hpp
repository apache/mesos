#ifndef __PROCESS_PROMISE_HPP__
#define __PROCESS_PROMISE_HPP__

#include <process/future.hpp>


namespace process {

template <typename T>
class Promise
{
public:
  Promise();
  Promise(const T& t);
  Promise(const Promise<T>& that);
  ~Promise();

  // Sets the value for this promise, unless the value has already
  // been set, in which case it returns false.
  bool set(const T& _t);

  // Returns a future associated with this promise.
  Future<T> future() const;

private:
  // Not assignable.
  void operator = (const Promise<T>&);

  Future<T> f;
};


template <>
class Promise<void>;


template <typename T>
class Promise<T&>;


template <typename T>
Promise<T>::Promise() {}


template <typename T>
Promise<T>::Promise(const T& t)
  : f(t) {}


template <typename T>
Promise<T>::Promise(const Promise<T>& that)
{
  f = that.f;
}


template <typename T>
Promise<T>::~Promise() {}


template <typename T>
bool Promise<T>::set(const T& t)
{
  return f.set(t);
}


template <typename T>
Future<T> Promise<T>::future() const
{
  return f;
}

}  // namespace process {

#endif // __PROCESS_PROMISE_HPP__
