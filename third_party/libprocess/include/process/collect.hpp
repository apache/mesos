#ifndef __PROCESS_COLLECT_HPP__
#define __PROCESS_COLLECT_HPP__

#include <assert.h>

#include <list>

#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

namespace process {

// Waits on each future in the specified set and returns the set of
// resulting values. If any future is discarded then the result will
// be a failure. Likewise, if any future fails than the result future
// will be a failure.
template <typename T>
Future<std::list<T> > collect(std::list<Future<T> >& futures);


namespace internal {

template <typename T>
class CollectProcess : public Process<CollectProcess<T> >
{
public:
  CollectProcess(
      const std::list<Future<T> >& _futures,
      Promise<std::list<T> >* _promise)
    : futures(_futures), promise(_promise) {}

  virtual ~CollectProcess()
  {
    delete promise;
  }

  virtual void initialize()
  {
    // Stop this nonsense if nobody cares.
    promise->future().onDiscarded(defer(this, &CollectProcess::discarded));

    typename std::list<Future<T> >::iterator iterator;
    for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
      const Future<T>& future = *iterator;
      future.onAny(defer(this, &CollectProcess::waited, future));
    }
  }

private:
  void discarded()
  {
    terminate(this);
  }

  void waited(const Future<T>& future)
  {
    if (future.isFailed()) {
      promise->fail("Collect failed: " + future.failure());
    } else if (future.isDiscarded()) {
      promise->fail("Collect failed: future discarded");
    } else {
      assert(future.isReady());
      values.push_back(future.get());
      if (futures.size() == values.size()) {
        promise->set(values);
        terminate(this);
      }
    }
  }

  std::list<Future<T> > futures;
  Promise<std::list<T> >* promise;
  std::list<T> values;
};

} // namespace internal {


template <typename T>
inline Future<std::list<T> > collect(std::list<Future<T> >& futures)
{
  Promise<std::list<T> >* promise = new Promise<std::list<T> >();
  spawn(new internal::CollectProcess<T>(futures, promise), true);
  return promise->future();
}

} // namespace process {

#endif // __PROCESS_COLLECT_HPP__
