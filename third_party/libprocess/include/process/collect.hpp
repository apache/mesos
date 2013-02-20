#ifndef __PROCESS_COLLECT_HPP__
#define __PROCESS_COLLECT_HPP__

#include <assert.h>

#include <list>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/none.hpp>
#include <stout/option.hpp>

namespace process {

// Waits on each future in the specified set and returns the set of
// resulting values. If any future is discarded then the result will
// be a failure. Likewise, if any future fails than the result future
// will be a failure.
template <typename T>
Future<std::list<T> > collect(
    std::list<Future<T> >& futures,
    const Option<Timeout>& timeout = None());


namespace internal {

template <typename T>
class CollectProcess : public Process<CollectProcess<T> >
{
public:
  CollectProcess(
      const std::list<Future<T> >& _futures,
      const Option<Timeout>& _timeout,
      Promise<std::list<T> >* _promise)
    : futures(_futures),
      timeout(_timeout),
      promise(_promise) {}

  virtual ~CollectProcess()
  {
    delete promise;
  }

  virtual void initialize()
  {
    // Stop this nonsense if nobody cares.
    promise->future().onDiscarded(defer(this, &CollectProcess::discarded));

    // Only wait as long as requested.
    if (timeout.isSome()) {
      delay(timeout.get().remaining(), this, &CollectProcess::timedout);
    }

    typename std::list<Future<T> >::const_iterator iterator;
    for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
      (*iterator).onAny(
          defer(this, &CollectProcess::waited, std::tr1::placeholders::_1));
    }
  }

private:
  void discarded()
  {
    terminate(this);
  }

  void timedout()
  {
    // Need to discard all of the futures so any of their associated
    // resources can get properly cleaned up.
    typename std::list<Future<T> >::const_iterator iterator;
    for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
      Future<T> future = *iterator; // Need a non-const copy to discard.
      future.discard();
    }

    promise->fail("Collect failed: timed out");
    terminate(this);
  }

  void waited(const Future<T>& future)
  {
    if (future.isFailed()) {
      promise->fail("Collect failed: " + future.failure());
      terminate(this);
    } else if (future.isDiscarded()) {
      promise->fail("Collect failed: future discarded");
      terminate(this);
    } else {
      assert(future.isReady());
      values.push_back(future.get());
      if (futures.size() == values.size()) {
        promise->set(values);
        terminate(this);
      }
    }
  }

  const std::list<Future<T> > futures;
  const Option<Timeout> timeout;
  Promise<std::list<T> >* promise;
  std::list<T> values;
};

} // namespace internal {


template <typename T>
inline Future<std::list<T> > collect(
    std::list<Future<T> >& futures,
    const Option<Timeout>& timeout)
{
  Promise<std::list<T> >* promise = new Promise<std::list<T> >();
  Future<std::list<T> > future = promise->future();
  spawn(new internal::CollectProcess<T>(futures, timeout, promise), true);
  return future;
}

} // namespace process {

#endif // __PROCESS_COLLECT_HPP__
