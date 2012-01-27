#ifndef __PROCESS_FUTURE_HPP__
#define __PROCESS_FUTURE_HPP__

#include <abort.h>
#include <assert.h>

#include <queue>
#include <set>

#include <tr1/functional>
#include <tr1/memory> // TODO(benh): Replace shared_ptr with unique_ptr.

#include <process/latch.hpp>
#include <process/option.hpp>

namespace process {

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
  bool await(double secs = 0) const;

  // Return the value associated with this future, waits indefinitely
  // until a value gets associated or until the future is discarded.
  T get() const;

  // Returns the failure message associated with this future.
  std::string failure() const;

  // Type of the callback functions that can get invoked when the
  // future gets set, fails, or is discarded.
  typedef std::tr1::function<void(const T&)> ReadyCallback;
  typedef std::tr1::function<void(const std::string&)> FailedCallback;
  typedef std::tr1::function<void()> DiscardedCallback;
  typedef std::tr1::function<void(const Future<T>&)> AnyCallback;

  // Installs callbacks for the specified events and returns a const
  // reference to 'this' in order to easily support chaining.
  const Future<T>& onReady(const ReadyCallback& callback) const;
  const Future<T>& onFailed(const FailedCallback& callback) const;
  const Future<T>& onDiscarded(const DiscardedCallback& callback) const;
  const Future<T>& onAny(const AnyCallback& callback) const;

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

  enum State {
    PENDING,
    READY,
    FAILED,
    DISCARDED,
  };

  int* refs;
  int* lock;
  State* state;
  T** t;
  std::string** message; // Message associated with failure.
  std::queue<ReadyCallback>* onReadyCallbacks;
  std::queue<FailedCallback>* onFailedCallbacks;
  std::queue<DiscardedCallback>* onDiscardedCallbacks;
  std::queue<AnyCallback>* onAnyCallbacks;
  Latch* latch;
};


// TODO(benh): Making Promise a subclass of Future?
template <typename T>
class Promise
{
public:
  Promise();
  Promise(const T& t);
  ~Promise();

  bool set(const T& _t);
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
    (*iterator).onAny(select);
  }

  return future;
}


template <typename T>
void discard(const std::set<Future<T> >& futures)
{
  typename std::set<Future<T> >::iterator iterator;
  for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
    Future<T> future = *iterator; // Need a non-const copy to discard.
    future.discard();
  }
}


template <typename T>
Future<T>::Future()
  : refs(new int(1)),
    lock(new int(0)),
    state(new State(PENDING)),
    t(new T*(NULL)),
    message(new std::string*(NULL)),
    onReadyCallbacks(new std::queue<ReadyCallback>()),
    onFailedCallbacks(new std::queue<FailedCallback>()),
    onDiscardedCallbacks(new std::queue<DiscardedCallback>()),
    onAnyCallbacks(new std::queue<AnyCallback>()),
    latch(new Latch()) {}


template <typename T>
Future<T>::Future(const T& _t)
  : refs(new int(1)),
    lock(new int(0)),
    state(new State(PENDING)),
    t(new T*(NULL)),
    message(new std::string*(NULL)),
    onReadyCallbacks(new std::queue<ReadyCallback>()),
    onFailedCallbacks(new std::queue<FailedCallback>()),
    onDiscardedCallbacks(new std::queue<DiscardedCallback>()),
    onAnyCallbacks(new std::queue<AnyCallback>()),
    latch(new Latch())
{
  set(_t);
}


template <typename T>
Future<T>::Future(const Future<T>& that)
{
  copy(that);
}


template <typename T>
Future<T>::~Future()
{
  cleanup();
}


template <typename T>
Future<T>& Future<T>::operator = (const Future<T>& that)
{
  if (this != &that) {
    cleanup();
    copy(that);
  }
  return *this;
}


template <typename T>
bool Future<T>::operator == (const Future<T>& that) const
{
  assert(latch != NULL);
  assert(that.latch != NULL);
  return *latch == *that.latch;
}


template <typename T>
bool Future<T>::operator < (const Future<T>& that) const
{
  assert(latch != NULL);
  assert(that.latch != NULL);
  return *latch < *that.latch;
}


template <typename T>
bool Future<T>::discard()
{
  bool result = false;

  assert(lock != NULL);
  internal::acquire(lock);
  {
    assert(state != NULL);
    if (*state == PENDING) {
      *state = DISCARDED;
      latch->trigger();
      result = true;
    }
  }
  internal::release(lock);

  // Invoke all callbacks associated with this future being
  // DISCARDED. We don't need a lock because the state is now in
  // DISCARDED so there should not be any concurrent modications.
  if (result) {
    while (!onDiscardedCallbacks->empty()) {
      // TODO(*): Invoke callbacks in another execution context.
      onDiscardedCallbacks->front()();
      onDiscardedCallbacks->pop();
    }

    while (!onAnyCallbacks->empty()) {
      // TODO(*): Invoke callbacks in another execution context.
      onAnyCallbacks->front()(*this);
      onAnyCallbacks->pop();
    }
  }

  return result;
}


template <typename T>
bool Future<T>::isPending() const
{
  assert(state != NULL);
  return *state == PENDING;
}


template <typename T>
bool Future<T>::isReady() const
{
  assert(state != NULL);
  return *state == READY;
}


template <typename T>
bool Future<T>::isDiscarded() const
{
  assert(state != NULL);
  return *state == DISCARDED;
}


template <typename T>
bool Future<T>::isFailed() const
{
  assert(state != NULL);
  return *state == FAILED;
}


template <typename T>
bool Future<T>::await(double secs) const
{
  if (!isReady() && !isDiscarded() && !isFailed()) {
    assert(latch != NULL);
    return latch->await(secs);
  }
  return true;
}


template <typename T>
T Future<T>::get() const
{
  if (!isReady()) {
    await();
  }

  if (!isReady()) {
    abort();
  }

  assert(t != NULL);
  assert(*t != NULL);
  return **t;
}


template <typename T>
std::string Future<T>::failure() const
{
  assert(message != NULL);
  if (*message != NULL) {
    return **message;
  }

  return "";
}


template <typename T>
const Future<T>& Future<T>::onReady(const ReadyCallback& callback) const
{
  bool run = false;

  assert(lock != NULL);
  internal::acquire(lock);
  {
    assert(state != NULL);
    if (*state == READY) {
      run = true;
    } else if (*state == PENDING) {
      onReadyCallbacks->push(callback);
    }
  }
  internal::release(lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(**t);
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onFailed(const FailedCallback& callback) const
{
  bool run = false;

  assert(lock != NULL);
  internal::acquire(lock);
  {
    assert(state != NULL);
    if (*state == FAILED) {
      run = true;
    } else if (*state == PENDING) {
      onFailedCallbacks->push(callback);
    }
  }
  internal::release(lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(**message);
  }

  return *this;
}


template <typename T>
const Future<T>& Future<T>::onDiscarded(
    const DiscardedCallback& callback) const
{
  bool run = false;

  assert(lock != NULL);
  internal::acquire(lock);
  {
    assert(state != NULL);
    if (*state == DISCARDED) {
      run = true;
    } else if (*state == PENDING) {
      onDiscardedCallbacks->push(callback);
    }
  }
  internal::release(lock);

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

  assert(lock != NULL);
  internal::acquire(lock);
  {
    assert(state != NULL);
    if (*state != PENDING) {
      run = true;
    } else if (*state == PENDING) {
      onAnyCallbacks->push(callback);
    }
  }
  internal::release(lock);

  // TODO(*): Invoke callback in another execution context.
  if (run) {
    callback(*this);
  }

  return *this;
}


template <typename T>
bool Future<T>::set(const T& _t)
{
  bool result = false;

  assert(lock != NULL);
  internal::acquire(lock);
  {
    assert(state != NULL);
    if (*state == PENDING) {
      *t = new T(_t);
      *state = READY;
      latch->trigger();
      result = true;
    }
  }
  internal::release(lock);

  // Invoke all callbacks associated with this future being READY. We
  // don't need a lock because the state is now in READY so there
  // should not be any concurrent modications.
  if (result) {
    while (!onReadyCallbacks->empty()) {
      // TODO(*): Invoke callbacks in another execution context.
      onReadyCallbacks->front()(**t);
      onReadyCallbacks->pop();
    }

    while (!onAnyCallbacks->empty()) {
      // TODO(*): Invoke callbacks in another execution context.
      onAnyCallbacks->front()(*this);
      onAnyCallbacks->pop();
    }
  }

  return result;
}


template <typename T>
bool Future<T>::fail(const std::string& _message)
{
  bool result = false;

  assert(lock != NULL);
  internal::acquire(lock);
  {
    assert(state != NULL);
    if (*state == PENDING) {
      *message = new std::string(_message);
      *state = FAILED;
      latch->trigger();
      result = true;
    }
  }
  internal::release(lock);

  // Invoke all callbacks associated with this future being FAILED. We
  // don't need a lock because the state is now in FAILED so there
  // should not be any concurrent modications.
  if (result) {
    while (!onFailedCallbacks->empty()) {
      // TODO(*): Invoke callbacks in another execution context.
      onFailedCallbacks->front()(**message);
      onFailedCallbacks->pop();
    }

    while (!onAnyCallbacks->empty()) {
      // TODO(*): Invoke callbacks in another execution context.
      onAnyCallbacks->front()(*this);
      onAnyCallbacks->pop();
    }
  }

  return result;
}


template <typename T>
void Future<T>::copy(const Future<T>& that)
{
  assert(that.refs > 0);
  __sync_fetch_and_add(that.refs, 1);
  refs = that.refs;
  lock = that.lock;
  state = that.state;
  t = that.t;
  message = that.message;
  onReadyCallbacks = that.onReadyCallbacks;
  onFailedCallbacks = that.onFailedCallbacks;
  onDiscardedCallbacks = that.onDiscardedCallbacks;
  onAnyCallbacks = that.onAnyCallbacks;
  latch = that.latch;
}


template <typename T>
void Future<T>::cleanup()
{
  assert(refs != NULL);
  if (__sync_sub_and_fetch(refs, 1) == 0) {
    // Discard the future if it is still pending (so we invoke any
    // discarded callbacks that have been setup). Note that we put the
    // reference count back at 1 here in case one of the callbacks
    // decides it wants to keep a reference.
    assert(state != NULL);
    if (*state == PENDING) {
      *refs = 1;
      discard();
    }

    // Now try and cleanup again (this time we know the future has
    // either been discarded or was not pending). Note that one of the
    // callbacks might have stored the future, in which case we'll
    // just return without doing anything, but the state will forever
    // be "discarded".
    assert(refs != NULL);
    if (__sync_sub_and_fetch(refs, 1) == 0) {
      delete refs;
      refs = NULL;
      assert(lock != NULL);
      delete lock;
      lock = NULL;
      assert(state != NULL);
      delete state;
      state = NULL;
      assert(t != NULL);
      delete *t;
      delete t;
      t = NULL;
      assert(message != NULL);
      delete *message;
      delete message;
      message = NULL;
      assert(onReadyCallbacks != NULL);
      delete onReadyCallbacks;
      onReadyCallbacks = NULL;
      assert(onFailedCallbacks != NULL);
      delete onFailedCallbacks;
      onFailedCallbacks = NULL;
      assert(onDiscardedCallbacks != NULL);
      delete onDiscardedCallbacks;
      onDiscardedCallbacks = NULL;
      assert(onAnyCallbacks != NULL);
      delete onAnyCallbacks;
      onAnyCallbacks = NULL;
      assert(latch != NULL);
      delete latch;
      latch = NULL;
    }
  }
}

}  // namespace process {

#endif // __PROCESS_FUTURE_HPP__
