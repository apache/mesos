#ifndef __PROMISE_HPP__
#define __PROMISE_HPP__

#include "future.hpp"


template <typename T>
class Promise
{
public:
  Promise();
  Promise(const T &t_);
  Promise(const Promise<T> &that);
  virtual ~Promise();
  void set(const T &t_);
  void associate(Future<T> *future_);

private:
  void operator = (const Promise<T> &);

  enum State {
    UNSET_UNASSOCIATED,
    SET_UNASSOCIATED,
    UNSET_ASSOCIATED,
    SET_ASSOCIATED,
  };

  int *refs;
  T **t;
  Future<T> **future;
  State *state;
};


template <>
class Promise<void>;


template <typename T>
class Promise<T&>;


template <typename T>
Promise<T>::Promise()
{
  refs = new int;
  *refs = 1;
  t = new T *;
  *t = NULL;
  future = new Future<T> *;
  *future = NULL;
  state = new State;
  *state = UNSET_UNASSOCIATED;
}

template <typename T>
Promise<T>::Promise(const T &_t)
{
  refs = new int;
  *refs = 1;
  t = new T *;
  *t = new T(_t);
  future = new Future<T> *;
  *future = NULL;
  state = new State;
  *state = SET_UNASSOCIATED;
}


template <typename T>
Promise<T>::Promise(const Promise<T> &that)
{
  assert(that.refs != NULL);
  assert(*that.refs > 0);
  __sync_fetch_and_add(that.refs, 1);
  refs = that.refs;
  t = that.t;
  state = that.state;
  future = that.future;
}


template <typename T>
Promise<T>::~Promise()
{
  assert(refs != NULL);
  if (__sync_sub_and_fetch(refs, 1) == 0) {
    delete refs;
    assert(t != NULL);
    if (*t != NULL)
      delete *t;
    assert(state != NULL);
    delete state;
    assert(future != NULL);
    if (*future != NULL)
      delete *future;
  }
}


template <typename T>
void Promise<T>::set(const T &t_)
{
  assert(state != NULL);
  assert(*state == UNSET_UNASSOCIATED ||
         *state == UNSET_ASSOCIATED);
  assert(t != NULL && *t == NULL);
  if (*state == UNSET_UNASSOCIATED) {
    *t = new T(t_);
    if (!__sync_bool_compare_and_swap(state, UNSET_UNASSOCIATED, SET_UNASSOCIATED)) {
      assert(*state == UNSET_ASSOCIATED);
      __sync_bool_compare_and_swap(state, UNSET_ASSOCIATED, SET_ASSOCIATED);
      assert(future != NULL && *future != NULL);
      (*future)->set(**t);
    }
  } else {
    assert(*state == UNSET_ASSOCIATED);
    assert(future != NULL && *future != NULL);
    (*future)->set(t_);
    __sync_bool_compare_and_swap(state, UNSET_ASSOCIATED, SET_ASSOCIATED);
  }
}


template <typename T>
void Promise<T>::associate(Future<T> *future_)
{
  assert(state != NULL);
  assert(*state == UNSET_UNASSOCIATED ||
         *state == SET_UNASSOCIATED);
  assert(future != NULL);
  *future = future_;
  if (*state == UNSET_UNASSOCIATED) {
    if (!__sync_bool_compare_and_swap(state, UNSET_UNASSOCIATED,
                                      UNSET_ASSOCIATED)) {
      assert(*state == SET_UNASSOCIATED);
      __sync_bool_compare_and_swap(state, SET_UNASSOCIATED, SET_ASSOCIATED);
      assert(*state == SET_ASSOCIATED);
      assert(t != NULL && *t != NULL);
      (*future)->set(**t);
    }
  } else {
    assert(*state == SET_UNASSOCIATED);
    __sync_bool_compare_and_swap(state, SET_UNASSOCIATED, SET_ASSOCIATED);
    assert(*state == SET_ASSOCIATED);
    assert(t != NULL && *t != NULL);
    (*future)->set(**t);
  }
}

#endif // __PROMISE_HPP__
