#ifndef __PROCESS_FUTURE_HPP__
#define __PROCESS_FUTURE_HPP__

#include <assert.h>
#include <stdlib.h> // For abort.

#include <iostream>
#include <list>
#include <queue>
#include <set>

#include <glog/logging.h>

#include <process/latch.hpp>
#include <process/pid.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/memory.hpp> // TODO(benh): Replace shared_ptr with unique_ptr.
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/preprocessor.hpp>

namespace process {

// Forward declaration (instead of include to break circular dependency).
template <typename _F> struct _Defer;
template <typename F> struct Deferred;
#if  __cplusplus >= 201103L
template <typename G> struct _Deferred;
#endif // __cplusplus >= 201103L

namespace internal {

template <typename T>
struct wrap;

template <typename T>
struct unwrap;

} // namespace internal {


// Forward declaration of Promise.
template <typename T>
class Promise;


// Forward declaration of WeakFuture.
template <typename T>
class WeakFuture;

// Forward declaration of Failure.
struct Failure;


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

  template <typename U>
  Future(const U& u);

  Future(const Failure& failure);

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
  typedef lambda::function<void(const T&)> ReadyCallback;
  typedef lambda::function<void(const std::string&)> FailedCallback;
  typedef lambda::function<void(void)> DiscardedCallback;
  typedef lambda::function<void(const Future<T>&)> AnyCallback;

#if __cplusplus >= 201103L
  // Installs callbacks for the specified events and returns a const
  // reference to 'this' in order to easily support chaining.
  const Future<T>& onReady(ReadyCallback&& callback) const;
  const Future<T>& onFailed(FailedCallback&& callback) const;
  const Future<T>& onDiscarded(DiscardedCallback&& callback) const;
  const Future<T>& onAny(AnyCallback&& callback) const;

  // TODO(benh): Add onReady, onFailed, onAny for _Deferred<F> where F
  // is not expected.

  template <typename F>
  const Future<T>& onReady(_Deferred<F>&& deferred) const
  {
    return onReady(std::function<void(const T&)>(deferred));
  }

  template <typename F>
  const Future<T>& onFailed(_Deferred<F>&& deferred) const
  {
    return onFailed(std::function<void(const std::string&)>(deferred));
  }

  template <typename F>
  const Future<T>& onDiscarded(_Deferred<F>&& deferred) const
  {
    return onDiscarded(std::function<void()>(deferred));
  }

  template <typename F>
  const Future<T>& onAny(_Deferred<F>&& deferred) const
  {
    return onAny(std::function<void(const Future<T>&)>(deferred));
  }

private:
  // We use the 'Prefer' and 'LessPrefer' structs as a way to prefer
  // one function over the other when doing SFINAE for the 'onReady',
  // 'onFailed', 'onAny', and 'then' functions. In each of these cases
  // we prefer calling the version of the functor that takes in an
  // argument (i.e., 'const T&' for 'onReady' and 'then' and 'const
  // std::string&' for 'onFailed'), but we allow functors that don't
  // care about the argument. We don't need to do this for
  // 'onDiscarded' because it doesn't take an argument.
  struct LessPrefer {};
  struct Prefer : LessPrefer {};

  template <typename F, typename = typename std::result_of<F(const T&)>::type>
  const Future<T>& onReady(F&& f, Prefer) const
  {
    return onReady(std::function<void(const T&)>(
        [=] (const T& t) mutable {
          f(t);
        }));
  }

  template <typename F, typename = typename std::result_of<F()>::type>
  const Future<T>& onReady(F&& f, LessPrefer) const
  {
    return onReady(std::function<void(const T&)>(
        [=] (const T&) mutable {
          f();
        }));
  }

  template <typename F, typename = typename std::result_of<F(const std::string&)>::type>
  const Future<T>& onFailed(F&& f, Prefer) const
  {
    return onFailed(std::function<void(const std::string&)>(
        [=] (const std::string& message) mutable {
          f(message);
        }));
  }

  template <typename F, typename = typename std::result_of<F()>::type>
  const Future<T>& onFailed(F&& f, LessPrefer) const
  {
    return onFailed(std::function<void(const std::string&)>(
        [=] (const std::string&) mutable {
          f();
        }));
  }

  template <typename F, typename = typename std::result_of<F(const Future<T>&)>::type>
  const Future<T>& onAny(F&& f, Prefer) const
  {
    return onAny(std::function<void(const Future<T>&)>(
        [=] (const Future<T>& future) {
          f(future);
        }));
  }

  template <typename F, typename = typename std::result_of<F()>::type>
  const Future<T>& onAny(F&& f, LessPrefer) const
  {
    return onAny(std::function<void(const Future<T>&)>(
        [=] (const Future<T>&) mutable {
          f();
        }));
  }

public:
  template <typename F>
  const Future<T>& onReady(F&& f) const
  {
    return onReady(std::forward<F>(f), Prefer());
  }

  template <typename F>
  const Future<T>& onFailed(F&& f) const
  {
    return onFailed(std::forward<F>(f), Prefer());
  }

  template <typename F>
  const Future<T>& onDiscarded(F&& f) const
  {
    return onDiscarded(std::function<void()>(
        [=] () mutable {
          f();
        }));
  }

  template <typename F>
  const Future<T>& onAny(F&& f) const
  {
    return onAny(std::forward<F>(f), Prefer());
  }

#else // __cplusplus >= 201103L

  // Installs callbacks for the specified events and returns a const
  // reference to 'this' in order to easily support chaining.
  const Future<T>& onReady(const ReadyCallback& callback) const;
  const Future<T>& onFailed(const FailedCallback& callback) const;
  const Future<T>& onDiscarded(const DiscardedCallback& callback) const;
  const Future<T>& onAny(const AnyCallback& callback) const;
#endif // __cplusplus >= 201103L

  // Installs callbacks that get executed when this future is ready
  // and associates the result of the callback with the future that is
  // returned to the caller (which may be of a different type).
  template <typename X>
  Future<X> then(const lambda::function<Future<X>(const T&)>& f) const;

  template <typename X>
  Future<X> then(const lambda::function<X(const T&)>& f) const;

  template <typename X>
  Future<X> then(const lambda::function<Future<X>()>& f) const
  {
    return then(lambda::function<Future<X>(const T&)>(lambda::bind(f)));
  }

  template <typename X>
  Future<X> then(const lambda::function<X()>& f) const
  {
    return then(lambda::function<X(const T&)>(lambda::bind(f)));
  }

#if __cplusplus >= 201103L
private:
  template <typename F, typename X = typename internal::unwrap<typename std::result_of<F(const T&)>::type>::type>
  Future<X> then(_Deferred<F>&& f, Prefer) const
  {
    // note the then<X> is necessary to not have an infinite loop with
    // then(F&& f)
    return then<X>(std::function<Future<X>(const T&)>(f));
  }

  template <typename F, typename X = typename internal::unwrap<typename std::result_of<F()>::type>::type>
  Future<X> then(_Deferred<F>&& f, LessPrefer) const
  {
    return then<X>(std::function<Future<X>()>(f));
  }

  template <typename F, typename X = typename internal::unwrap<typename std::result_of<F(const T&)>::type>::type>
  Future<X> then(F&& f, Prefer) const
  {
    return then<X>(std::function<Future<X>(const T&)>(f));
  }

  template <typename F, typename X = typename internal::unwrap<typename std::result_of<F()>::type>::type>
  Future<X> then(F&& f, LessPrefer) const
  {
    return then<X>(std::function<Future<X>()>(f));
  }

public:
  template <typename F>
  auto then(F&& f) const
    -> decltype(this->then(std::forward<F>(f), Prefer()))
  {
    return then(std::forward<F>(f), Prefer());
  }

#else // __cplusplus >= 201103L

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
#endif // __cplusplus >= 201103L

private:
  friend class Promise<T>;
  friend class WeakFuture<T>;

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
    Latch* latch;
    State state;
    T* t;
    std::string* message; // Message associated with failure.
    std::queue<ReadyCallback> onReadyCallbacks;
    std::queue<FailedCallback> onFailedCallbacks;
    std::queue<DiscardedCallback> onDiscardedCallbacks;
    std::queue<AnyCallback> onAnyCallbacks;
  };

  // Sets the value for this future, unless the future is already set,
  // failed, or discarded, in which case it returns false.
  bool set(const T& _t);

  // Sets this future as failed, unless the future is already set,
  // failed, or discarded, in which case it returns false.
  bool fail(const std::string& _message);

  memory::shared_ptr<Data> data;
};


// Represents a weak reference to a future. This class is used to
// break cyclic dependencies between futures.
template <typename T>
class WeakFuture
{
public:
  WeakFuture(const Future<T>& future);

  // Converts this weak reference to a concrete future. Returns none
  // if the conversion is not successful.
  Option<Future<T> > get();

private:
  memory::weak_ptr<typename Future<T>::Data> data;
};


template <typename T>
WeakFuture<T>::WeakFuture(const Future<T>& future)
  : data(future.data) {}


template <typename T>
Option<Future<T> > WeakFuture<T>::get()
{
  Future<T> future;
  future.data = data.lock();

  if (future.data) {
    return future;
  } else {
    return None();
  }
}


namespace internal {

// Discards a weak future. If the weak future is invalid (i.e., the
// future it references to has already been destroyed), this operation
// is treated as a no-op.
template <typename T>
void discard(WeakFuture<T> reference)
{
  Option<Future<T> > future = reference.get();
  if (future.isSome()) {
    future.get().discard();
  }
}

} // namespace internal {


// Helper for creating failed futures.
struct Failure
{
  Failure(const std::string& _message) : message(_message) {}
  Failure(const Error& error) : message(error.message) {}

  const std::string message;
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
  // TODO(jieyu): Make 'f' a true alias of 'future'. Currently, only
  // 'discard' is associated in both directions. In other words, if a
  // future gets discarded, the other future will also get discarded.
  // For 'set' and 'fail', they are associated only in one direction.
  // In other words, calling 'set' or 'fail' on this promise will not
  // affect the result of the future that we associated. To avoid
  // cyclic dependencies, we keep a weak future in the callback.
  f.onDiscarded(lambda::bind(&internal::discard<T>, WeakFuture<T>(future)));

  if (!f.isPending()) {
    return false;
  }

  future
    .onReady(lambda::bind(&Future<T>::set, f, lambda::_1))
    .onFailed(lambda::bind(&Future<T>::fail, f, lambda::_1))
    .onDiscarded(lambda::bind(&Future<T>::discard, f));

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
  typedef Future<T> type;
};


template <typename X>
struct wrap<Future<X> >
{
  typedef Future<X> type;
};


template <typename T>
struct unwrap
{
  typedef T type;
};


template <typename X>
struct unwrap<Future<X> >
{
  typedef X type;
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
    memory::shared_ptr<Promise<Future<T > > > promise)
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
  memory::shared_ptr<Promise<Future<T> > > promise(
      new Promise<Future<T> >());

#if __cplusplus >= 201103L
  typename std::set<Future<T>>::iterator iterator;
  for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
    // NOTE: We can't use std::bind with a std::function with Clang
    // like we do below (see
    // http://stackoverflow.com/questions/20097616/stdbind-to-a-stdfunction-crashes-with-clang).
    (*iterator).onAny([=] (const Future<T>& future) {
      internal::select(future, promise);
    });
  }
#else // __cplusplus >= 201103L
  lambda::function<void(const Future<T>&)> select =
    lambda::bind(&internal::select<T>, lambda::_1, promise);

  typename std::set<Future<T> >::iterator iterator;
  for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
    (*iterator).onAny(lambda::bind(select, lambda::_1));
  }
#endif // __cplusplus >= 201103L

  return promise->future();
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
    latch(NULL),
    state(PENDING),
    t(NULL),
    message(NULL) {}


template <typename T>
Future<T>::Data::~Data()
{
  delete latch;
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
template <typename U>
Future<T>::Future(const U& u)
  : data(new Data())
{
  set(u);
}


template <typename T>
Future<T>::Future(const Failure& failure)
  : data(new Data())
{
  fail(failure.message);
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
      if (data->latch != NULL) {
        data->latch->trigger();
      }
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
  bool await = false;

  internal::acquire(&data->lock);
  {
    if (data->state == PENDING) {
      if (data->latch == NULL) {
        data->latch = new Latch();
      }
      await = true;
    }
  }
  internal::release(&data->lock);

  if (await) {
    return data->latch->await(duration);
  }

  return true;
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


#if __cplusplus >= 201103L
template <typename T>
const Future<T>& Future<T>::onReady(ReadyCallback&& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == READY) {
      run = true;
    } else if (data->state == PENDING) {
      data->onReadyCallbacks.push(std::move(callback));
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
const Future<T>& Future<T>::onFailed(FailedCallback&& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == FAILED) {
      run = true;
    } else if (data->state == PENDING) {
      data->onFailedCallbacks.push(std::move(callback));
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
const Future<T>& Future<T>::onDiscarded(DiscardedCallback&& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state == DISCARDED) {
      run = true;
    } else if (data->state == PENDING) {
      data->onDiscardedCallbacks.push(std::move(callback));
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
const Future<T>& Future<T>::onAny(AnyCallback&& callback) const
{
  bool run = false;

  internal::acquire(&data->lock);
  {
    if (data->state != PENDING) {
      run = true;
    } else if (data->state == PENDING) {
      data->onAnyCallbacks.push(std::move(callback));
    }
  }
  internal::release(&data->lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(*this);
  }

  return *this;
}

#else // __cplusplus >= 201103L
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
#endif // __cplusplus >= 201103L

namespace internal {

template <typename T, typename X>
void thenf(const memory::shared_ptr<Promise<X> >& promise,
           const lambda::function<Future<X>(const T&)>& f,
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
void then(const memory::shared_ptr<Promise<X> >& promise,
          const lambda::function<X(const T&)>& f,
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
Future<X> Future<T>::then(const lambda::function<Future<X>(const T&)>& f) const
{
  memory::shared_ptr<Promise<X> > promise(new Promise<X>());

  lambda::function<void(const Future<T>&)> thenf =
    lambda::bind(&internal::thenf<T, X>, promise, f, lambda::_1);

  onAny(thenf);

  // Propagate discarding up the chain. To avoid cyclic dependencies,
  // we keep a weak future in the callback.
  promise->future().onDiscarded(
      lambda::bind(&internal::discard<T>, WeakFuture<T>(*this)));

  return promise->future();
}


template <typename T>
template <typename X>
Future<X> Future<T>::then(const lambda::function<X(const T&)>& f) const
{
  memory::shared_ptr<Promise<X> > promise(new Promise<X>());

  lambda::function<void(const Future<T>&)> then =
    lambda::bind(&internal::then<T, X>, promise, f, lambda::_1);

  onAny(then);

  // Propagate discarding up the chain. To avoid cyclic dependencies,
  // we keep a weak future in the callback.
  promise->future().onDiscarded(
      lambda::bind(&internal::discard<T>, WeakFuture<T>(*this)));

  return promise->future();
}


template <typename T>
bool Future<T>::set(const T& _t)
{
  bool result = false;

  internal::acquire(&data->lock);
  {
    if (data->state == PENDING) {
      data->t = new T(_t);
      data->state = READY;
      if (data->latch != NULL) {
        data->latch->trigger();
      }
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
      if (data->latch != NULL) {
        data->latch->trigger();
      }
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
