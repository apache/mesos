#ifndef __PROCESS_FUTURE_HPP__
#define __PROCESS_FUTURE_HPP__

#include <assert.h>
#include <stdlib.h> // For abort.

#include <iostream>
#include <list>
#include <queue>
#include <set>

#include <glog/logging.h>

#include <tr1/functional>
#include <tr1/memory> // TODO(benh): Replace shared_ptr with unique_ptr.

#include <process/latch.hpp>
#include <process/pid.hpp>

#include <stout/duration.hpp>
#include <stout/option.hpp>
#include <stout/preprocessor.hpp>

namespace process {

// Forward declaration (instead of include to break circular dependency).
template <typename _F> struct _Defer;

namespace internal {

template <typename T>
struct wrap;

template <typename T>
struct unwrap;

} // namespace internal {


// Forward declaration of Promise.
template <typename T>
class Promise;


// Definition of a "shared" future. A future can hold any
// copy-constructible value. A future is considered "shared" because
// by default a future can be accessed concurrently.
template <typename T>
class Future
{
public:
  // Constructs a failed future.
  static Future<T> failed(const std::string& message);

  Future();
  Future(const T& _t);
  Future(const Future<T>& that);
  ~Future();

  // Futures are assignable (and copyable). This results in the
  // reference to the previous future data being decremented and a
  // reference to 'that' being incremented.
  Future<T>& operator = (const Future<T>& that);

  // Comparision operators useful for using futures in collections.
  bool operator == (const Future<T>& that) const;
  bool operator < (const Future<T>& that) const;

  // Helpers to get the current state of this future.
  bool isPending() const;
  bool isReady() const;
  bool isDiscarded() const;
  bool isFailed() const;

  // Discards this future. This is similar to cancelling a future,
  // however it also occurs when the last reference to this future
  // gets cleaned up. Returns false if the future could not be
  // discarded (for example, because it is ready or failed).
  bool discard();

  // Waits for this future to become ready, discarded, or failed.
  bool await(const Duration& duration = Seconds(-1)) const;

  // Return the value associated with this future, waits indefinitely
  // until a value gets associated or until the future is discarded.
  T get() const;

  // Returns the failure message associated with this future.
  std::string failure() const;

  // Type of the callback functions that can get invoked when the
  // future gets set, fails, or is discarded.
  typedef std::tr1::function<void(const T&)> ReadyCallback;
  typedef std::tr1::function<void(const std::string&)> FailedCallback;
  typedef std::tr1::function<void(void)> DiscardedCallback;
  typedef std::tr1::function<void(const Future<T>&)> AnyCallback;

  // Installs callbacks for the specified events and returns a const
  // reference to 'this' in order to easily support chaining.
  const Future<T>& onReady(const ReadyCallback& callback) const;
  const Future<T>& onFailed(const FailedCallback& callback) const;
  const Future<T>& onDiscarded(const DiscardedCallback& callback) const;
  const Future<T>& onAny(const AnyCallback& callback) const;

  // Installs callbacks that get executed when this future is ready
  // and associates the result of the callback with the future that is
  // returned to the caller (which may be of a different type).
  template <typename X>
  Future<X> then(const std::tr1::function<Future<X>(const T&)>& f) const;

  template <typename X>
  Future<X> then(const std::tr1::function<X(const T&)>& f) const;

  // Helpers for the compiler to be able to forward std::tr1::bind results.
  template <typename X>
  Future<X> then(const std::tr1::_Bind<X(*(void))(void)>& b) const
  {
    return then(std::tr1::function<X(const T&)>(b));
  }

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename X,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<X> then(                                                       \
      const std::tr1::_Bind<X(*(ENUM_PARAMS(N, A)))                     \
      (ENUM_PARAMS(N, P))>& b) const                                    \
  {                                                                     \
    return then(std::tr1::function<X(const T&)>(b));                    \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  template <typename X>
  Future<X> then(const std::tr1::_Bind<Future<X>(*(void))(void)>& b) const
  {
    return then(std::tr1::function<Future<X>(const T&)>(b));
  }

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename X,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<X> then(                                                       \
      const std::tr1::_Bind<Future<X>(*(ENUM_PARAMS(N, A)))             \
      (ENUM_PARAMS(N, P))>& b) const                                    \
  {                                                                     \
    return then(std::tr1::function<Future<X>(const T&)>(b));            \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  // Helpers for the compiler to be able to forward 'defer' results.
  template <typename X, typename U>
  Future<X> then(const _Defer<Future<X>(*(PID<U>, X(U::*)(void)))
                 (const PID<U>&, X(U::*)(void))>& d) const
  {
    return then(std::tr1::function<Future<X>(const T&)>(d));
  }

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename X,                                                 \
            typename U,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<X> then(                                                       \
      const _Defer<Future<X>(*(PID<U>,                                  \
                               X(U::*)(ENUM_PARAMS(N, P)),              \
                               ENUM_PARAMS(N, A)))                      \
      (const PID<U>&,                                                   \
       X(U::*)(ENUM_PARAMS(N, P)),                                      \
       ENUM_PARAMS(N, P))>& d) const                                    \
  {                                                                     \
    return then(std::tr1::function<Future<X>(const T&)>(d));            \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  template <typename X, typename U>
  Future<X> then(const _Defer<Future<X>(*(PID<U>, Future<X>(U::*)(void)))
                 (const PID<U>&, Future<X>(U::*)(void))>& d) const
  {
    return then(std::tr1::function<Future<X>(const T&)>(d));
  }

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename X,                                                 \
            typename U,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<X> then(                                                       \
      const _Defer<Future<X>(*(PID<U>,                                  \
                               Future<X>(U::*)(ENUM_PARAMS(N, P)),      \
                               ENUM_PARAMS(N, A)))                      \
      (const PID<U>&,                                                   \
       Future<X>(U::*)(ENUM_PARAMS(N, P)),                              \
       ENUM_PARAMS(N, P))>& d) const                                    \
  {                                                                     \
    return then(std::tr1::function<Future<X>(const T&)>(d));            \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  // C++11 implementation (covers all functors).
#if __cplusplus >= 201103L
  template <typename F>
  auto then(F f) const
    -> typename internal::wrap<decltype(f(T()))>::Type;
#endif

private:
  friend class Promise<T>;

  // Sets the value for this future, unless the future is already set,
  // failed, or discarded, in which case it returns false.
  bool set(const T& _t);

  // Sets this future as failed, unless the future is already set,
  // failed, or discarded, in which case it returns false.
  bool fail(const std::string& _message);

  void copy(const Future<T>& that);
  void cleanup();

  enum State
  {
    PENDING,
    READY,
    FAILED,
    DISCARDED,
  };

  struct Data
  {
    Data();
    ~Data();

    int lock;
    Latch latch;
    State state;
    T* t;
    std::string* message; // Message associated with failure.
    std::queue<ReadyCallback> onReadyCallbacks;
    std::queue<FailedCallback> onFailedCallbacks;
    std::queue<DiscardedCallback> onDiscardedCallbacks;
    std::queue<AnyCallback> onAnyCallbacks;
  };

  std::tr1::shared_ptr<Data> data;
};


// TODO(benh): Make Promise a subclass of Future?
template <typename T>
class Promise
{
public:
  Promise();
  Promise(const T& t);
  virtual ~Promise();

  bool set(const T& _t);
  bool set(const Future<T>& future); // Alias for associate.
  bool associate(const Future<T>& future);
  bool fail(const std::string& message);

  // Returns a copy of the future associated with this promise.
  Future<T> future() const;

private:
  // Not copyable, not assignable.
  Promise(const Promise<T>&);
  Promise<T>& operator = (const Promise<T>&);

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
Promise<T>::~Promise() {}


template <typename T>
bool Promise<T>::set(const T& t)
{
  return f.set(t);
}


template <typename T>
bool Promise<T>::set(const Future<T>& future)
{
  return associate(future);
}


template <typename T>
bool Promise<T>::associate(const Future<T>& future)
{
  if (!f.isPending()) {
    return false;
  }

  future
    .onReady(std::tr1::bind(&Future<T>::set, f, std::tr1::placeholders::_1))
    .onFailed(std::tr1::bind(&Future<T>::fail, f, std::tr1::placeholders::_1))
    .onDiscarded(std::tr1::bind(&Future<T>::discard, f));

  return true;
}


template <typename T>
bool Promise<T>::fail(const std::string& message)
{
  return f.fail(message);
}


template <typename T>
Future<T> Promise<T>::future() const
{
  return f;
}


// Internal helper utilities.
namespace internal {

template <typename T>
struct wrap
{
  typedef Future<T> Type;
};


template <typename X>
struct wrap<Future<X> >
{
  typedef Future<X> Type;
};


template <typename T>
struct unwrap
{
  typedef T Type;
};


template <typename X>
struct unwrap<Future<X> >
{
  typedef X Type;
};


inline void acquire(int* lock)
{
  while (!__sync_bool_compare_and_swap(lock, 0, 1)) {
    asm volatile ("pause");
  }
}


inline void release(int* lock)
{
  // Unlock via a compare-and-swap so we get a memory barrier too.
  bool unlocked = __sync_bool_compare_and_swap(lock, 1, 0);
  assert(unlocked);
}


template <typename T>
void select(
    const Future<T>& future,
    std::tr1::shared_ptr<Promise<Future<T > > > promise)
{
  // We never fail the future associated with our promise.
  assert(!promise->future().isFailed());

  if (promise->future().isPending()) { // No-op if it's discarded.
    if (future.isReady()) { // We only set the promise if a future is ready.
      promise->set(future);
    }
  }
}

} // namespace internal {


// TODO(benh): Move select and discard into 'futures' namespace.

// Returns a future that captures any ready future in a set. Note that
// select DOES NOT capture a future that has failed or been discarded.
template <typename T>
Future<Future<T> > select(const std::set<Future<T> >& futures)
{
  std::tr1::shared_ptr<Promise<Future<T> > > promise(
      new Promise<Future<T> >());

  Future<Future<T> > future = promise->future();

  std::tr1::function<void(const Future<T>&)> select =
    std::tr1::bind(&internal::select<T>,
                   std::tr1::placeholders::_1,
                   promise);

  typename std::set<Future<T> >::iterator iterator;
  for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
    (*iterator).onAny(std::tr1::bind(select, std::tr1::placeholders::_1));
  }

  return future;
}


template <typename T>
void discard(const std::set<Future<T> >& futures)
{
  typename std::set<Future<T> >::const_iterator iterator;
  for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
    Future<T> future = *iterator; // Need a non-const copy to discard.
    future.discard();
  }
}


template <typename T>
void discard(const std::list<Future<T> >& futures)
{
  typename std::list<Future<T> >::const_iterator iterator;
  for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
    Future<T> future = *iterator; // Need a non-const copy to discard.
    future.discard();
  }
}


template <class T>
void fail(const std::vector<Promise<T>*>& promises, const std::string& message)
{
  typename std::vector<Promise<T>*>::const_iterator iterator;
  for (iterator = promises.begin(); iterator != promises.end(); ++iterator) {
    Promise<T>* promise = *iterator;
    promise->fail(message);
  }
}


template <class T>
void fail(const std::list<Promise<T>*>& promises, const std::string& message)
{
  typename std::list<Promise<T>*>::const_iterator iterator;
  for (iterator = promises.begin(); iterator != promises.end(); ++iterator) {
    Promise<T>* promise = *iterator;
    promise->fail(message);
  }
}


template <typename T>
Future<T> Future<T>::failed(const std::string& message)
{
  Future<T> future;
  future.fail(message);
  return future;
}


template <typename T>
Future<T>::Data::Data()
  : lock(0),
    state(PENDING),
    t(NULL),
    message(NULL) {}


template <typename T>
Future<T>::Data::~Data()
{
  delete t;
  delete message;
}


template <typename T>
Future<T>::Future()
  : data(new Data()) {}


template <typename T>
Future<T>::Future(const T& _t)
  : data(new Data())
{
  set(_t);
}


template <typename T>
Future<T>::Future(const Future<T>& that)
  : data(that.data) {}


template <typename T>
Future<T>::~Future()
{
  if (data.unique()) {
    discard();
  }
}


template <typename T>
Future<T>& Future<T>::operator = (const Future<T>& that)
{
  if (this != &that) {
    if (data.unique()) {
      discard();
    }
    data = that.data;
  }
  return *this;
}


template <typename T>
bool Future<T>::operator == (const Future<T>& that) const
{
  return data == that.data;
}


template <typename T>
bool Future<T>::operator < (const Future<T>& that) const
{
  return data < that.data;
}


template <typename T>
bool Future<T>::discard()
{
  bool result = false;

  internal::acquire(&data->lock);
  {
    if (data->state == PENDING) {
      data->state = DISCARDED;
      data->latch.trigger();
      result = true;
    }
  }
  internal::release(&data->lock);

  // Invoke all callbacks associated with this future being
  // DISCARDED. We don't need a lock because the state is now in
  // DISCARDED so there should not be any concurrent modifications.
  if (result) {
    while (!data->onDiscardedCallbacks.empty()) {
      // TODO(*): Invoke callbacks in another execution context.
      data->onDiscardedCallbacks.front()();
      data->onDiscardedCallbacks.pop();
    }

    while (!data->onAnyCallbacks.empty()) {
      // TODO(*): Invoke callbacks in another execution context.
      data->onAnyCallbacks.front()(*this);
      data->onAnyCallbacks.pop();
    }
  }

  return result;
}


template <typename T>
bool Future<T>::isPending() const
{
  return data->state == PENDING;
}


template <typename T>
bool Future<T>::isReady() const
{
  return data->state == READY;
}


template <typename T>
bool Future<T>::isDiscarded() const
{
  return data->state == DISCARDED;
}


template <typename T>
bool Future<T>::isFailed() const
{
  return data->state == FAILED;
}


template <typename T>
bool Future<T>::await(const Duration& duration) const
{
  if (!isReady() && !isDiscarded() && !isFailed()) {
    return data->latch.await(duration);
  } else {
    return true;
  }
}


template <typename T>
T Future<T>::get() const
{
  if (!isReady()) {
    await();
  }

  CHECK(!isPending()) << "Future was in PENDING after await()";

  if (!isReady()) {
    if (isFailed()) {
      std::cerr << "Future::get() but state == FAILED: "
                << failure()  << std::endl;
    } else if (isDiscarded()) {
      std::cerr << "Future::get() but state == DISCARDED" << std::endl;
    }
    abort();
  }

  assert(data->t != NULL);
  return *data->t;
}


template <typename T>
std::string Future<T>::failure() const
{
  if (data->message != NULL) {
    return *data->message;
  }
  return "";
}


template <typename T>
const Future<T>& Future<T>::onReady(const ReadyCallback& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == READY) {
      run = true;
    } else if (data->state == PENDING) {
      data->onReadyCallbacks.push(callback);
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(*data->t);
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onFailed(const FailedCallback& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == FAILED) {
      run = true;
    } else if (data->state == PENDING) {
      data->onFailedCallbacks.push(callback);
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(*data->message);
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onDiscarded(
    const DiscardedCallback& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == DISCARDED) {
      run = true;
    } else if (data->state == PENDING) {
      data->onDiscardedCallbacks.push(callback);
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback();
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onAny(const AnyCallback& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state != PENDING) {
      run = true;
    } else if (data->state == PENDING) {
      data->onAnyCallbacks.push(callback);
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(*this);
  }

  return *this;
}


namespace internal {

template <typename T, typename X>
void thenf(const std::tr1::shared_ptr<Promise<X> >& promise,
           const std::tr1::function<Future<X>(const T&)>& f,
           const Future<T>& future)
{
  if (future.isReady()) {
    promise->associate(f(future.get()));
  } else if (future.isFailed()) {
    promise->fail(future.failure());
  } else if (future.isDiscarded()) {
    promise->future().discard();
  }
}


template <typename T, typename X>
void then(const std::tr1::shared_ptr<Promise<X> >& promise,
          const std::tr1::function<X(const T&)>& f,
          const Future<T>& future)
{
  if (future.isReady()) {
    promise->set(f(future.get()));
  } else if (future.isFailed()) {
    promise->fail(future.failure());
  } else if (future.isDiscarded()) {
    promise->future().discard();
  }
}

} // namespace internal {


template <typename T>
template <typename X>
Future<X> Future<T>::then(const std::tr1::function<Future<X>(const T&)>& f) const
{
  std::tr1::shared_ptr<Promise<X> > promise(new Promise<X>());

  std::tr1::function<void(const Future<T>&)> thenf =
    std::tr1::bind(&internal::thenf<T, X>,
                   promise,
                   f,
                   std::tr1::placeholders::_1);

  onAny(thenf);

  // Propagate discarding up the chain (note that we bind with a copy
  // of this future since 'this' might no longer be valid but other
  // references might still exist.
  // TODO(benh): Need to pass 'future' as a weak_ptr so that we can
  // avoid reference counting cycles!
  std::tr1::function<void(void)> discard =
    std::tr1::bind(&Future<T>::discard, *this);

  promise->future().onDiscarded(discard);

  return promise->future();
}


template <typename T>
template <typename X>
Future<X> Future<T>::then(const std::tr1::function<X(const T&)>& f) const
{
  std::tr1::shared_ptr<Promise<X> > promise(new Promise<X>());

  std::tr1::function<void(const Future<T>&)> then =
    std::tr1::bind(&internal::then<T, X>,
                   promise,
                   f,
                   std::tr1::placeholders::_1);

  onAny(then);

  // Propagate discarding up the chain (note that we bind with a copy
  // of this future since 'this' might no longer be valid but other
  // references might still exist.
  // TODO(benh): Need to pass 'future' as a weak_ptr so that we can
  // avoid reference counting cycles!
  std::tr1::function<void(void)> discard =
    std::tr1::bind(&Future<T>::discard, *this);

  promise->future().onDiscarded(discard);

  return promise->future();
}


#if __cplusplus >= 201103L
template <typename T>
template <typename F>
auto Future<T>::then(F f) const
  -> typename internal::wrap<decltype(f(T()))>::Type
{
  typedef typename internal::unwrap<decltype(f(T()))>::Type X;

  std::tr1::shared_ptr<Promise<X>> promise(new Promise<X>());

  onAny([=] (const Future<T>& future) {
    if (future.isReady()) {
      promise->set(f(future.get()));
    } else if (future.isFailed()) {
      promise->fail(future.failure());
    } else if (future.isDiscarded()) {
      promise->future().discard();
    }
  });

  // TODO(benh): Need to use weak_ptr here so that we can avoid
  // reference counting cycles!
  Future<T> future(*this);

  promise->future().onDiscarded([=] () {
    future.discard(); // Need a non-const copy to discard.
  });

  return promise->future();
}
#endif


template <typename T>
bool Future<T>::set(const T& _t)
{
  bool result = false;

  internal::acquire(&data->lock);
  {
    if (data->state == PENDING) {
      data->t = new T(_t);
      data->state = READY;
      data->latch.trigger();
      result = true;
    }
  }
  internal::release(&data->lock);

  // Invoke all callbacks associated with this future being READY. We
  // don't need a lock because the state is now in READY so there
  // should not be any concurrent modications.
  if (result) {
    while (!data->onReadyCallbacks.empty()) {
      // TODO(*): Invoke callbacks in another execution context.
      data->onReadyCallbacks.front()(*data->t);
      data->onReadyCallbacks.pop();
    }

    while (!data->onAnyCallbacks.empty()) {
      // TODO(*): Invoke callbacks in another execution context.
      data->onAnyCallbacks.front()(*this);
      data->onAnyCallbacks.pop();
    }
  }

  return result;
}


template <typename T>
bool Future<T>::fail(const std::string& _message)
{
  bool result = false;

  internal::acquire(&data->lock);
  {
    if (data->state == PENDING) {
      data->message = new std::string(_message);
      data->state = FAILED;
      data->latch.trigger();
      result = true;
    }
  }
  internal::release(&data->lock);

  // Invoke all callbacks associated with this future being FAILED. We
  // don't need a lock because the state is now in FAILED so there
  // should not be any concurrent modications.
  if (result) {
    while (!data->onFailedCallbacks.empty()) {
      // TODO(*): Invoke callbacks in another execution context.
      data->onFailedCallbacks.front()(*data->message);
      data->onFailedCallbacks.pop();
    }

    while (!data->onAnyCallbacks.empty()) {
      // TODO(*): Invoke callbacks in another execution context.
      data->onAnyCallbacks.front()(*this);
      data->onAnyCallbacks.pop();
    }
  }

  return result;
}

}  // namespace process {

#endif // __PROCESS_FUTURE_HPP__
