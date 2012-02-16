#ifndef __PROCESS_COLLECT_HPP__
#define __PROCESS_COLLECT_HPP__

#include <assert.h>

#include <set>

#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

namespace process {

// Transforms a set of futures of type T into a set of T's.
template <typename T>
Future<std::set<T> > collect(std::set<Future<T> >& futures);


namespace internal {

template <typename T>
class CollectProcess : public Process<CollectProcess<T> >
{
public:
  CollectProcess(
      const std::set<Future<T> >& _futures,
      Promise<std::set<T> >* _promise)
    : futures(_futures), promise(_promise) {}

  virtual ~CollectProcess()
  {
    delete promise;
  }

  virtual void initialize()
  {
    // Stop this nonsense if nobody cares.
    promise->future().onDiscarded(defer(this, &CollectProcess::discarded));

    typename std::set<Future<T> >::iterator iterator;
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
      promise->fail(future.failure());
    } else if (future.isDiscarded()) {
      promise->future().discard();
    } else {
      assert(future.isReady());
      values.insert(future.get());
      if (futures.size() == values.size()) {
        promise->set(values);
        terminate(this);
      }
    }
  }

  std::set<Future<T> > futures;
  Promise<std::set<T> >* promise;
  std::set<T> values;
};

} // namespace internal {


template <typename T>
inline Future<std::set<T> > collect(std::set<Future<T> >& futures)
{
  Promise<std::set<T> >* promise = new Promise<std::set<T> >();
  spawn(new internal::CollectProcess<T>(futures, promise), true);
  return promise->future();
}

} // namespace process {

#endif // __PROCESS_COLLECT_HPP__
