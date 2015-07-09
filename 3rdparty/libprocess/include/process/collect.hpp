/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#ifndef __PROCESS_COLLECT_HPP__
#define __PROCESS_COLLECT_HPP__

#include <list>
#include <tuple>

#include <process/check.hpp>
#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/lambda.hpp>

// TODO(bmahler): Move these into a futures.hpp header to group Future
// related utilities.

namespace process {

// Waits on each future in the specified list and returns the list of
// resulting values in the same order. If any future is discarded then
// the result will be a failure. Likewise, if any future fails then
// the result future will be a failure.
template <typename T>
Future<std::list<T>> collect(const std::list<Future<T>>& futures);


// Waits on each future in the specified set and returns the list of
// non-pending futures.
template <typename T>
Future<std::list<Future<T>>> await(const std::list<Future<T>>& futures);


// Waits on each future specified and returns the wrapping future
// typed of a tuple of futures.
// TODO(jieyu): Investigate the use of variadic templates here.
template <typename T1, typename T2>
Future<std::tuple<Future<T1>, Future<T2>>> await(
    const Future<T1>& future1,
    const Future<T2>& future2);


template <typename T1, typename T2, typename T3>
Future<std::tuple<Future<T1>, Future<T2>, Future<T3>>> await(
    const Future<T1>& future1,
    const Future<T2>& future2,
    const Future<T3>& future3);


namespace internal {

template <typename T>
class CollectProcess : public Process<CollectProcess<T>>
{
public:
  CollectProcess(
      const std::list<Future<T>>& _futures,
      Promise<std::list<T>>* _promise)
    : futures(_futures),
      promise(_promise),
      ready(0) {}

  virtual ~CollectProcess()
  {
    delete promise;
  }

  virtual void initialize()
  {
    // Stop this nonsense if nobody cares.
    promise->future().onDiscard(defer(this, &CollectProcess::discarded));

    typename std::list<Future<T>>::const_iterator iterator;
    for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
      (*iterator).onAny(defer(this, &CollectProcess::waited, lambda::_1));
    }
  }

private:
  void discarded()
  {
    promise->discard();
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
      CHECK_READY(future);
      ready += 1;
      if (ready == futures.size()) {
        std::list<T> values;
        foreach (const Future<T>& future, futures) {
          values.push_back(future.get());
        }
        promise->set(values);
        terminate(this);
      }
    }
  }

  const std::list<Future<T>> futures;
  Promise<std::list<T>>* promise;
  size_t ready;
};


template <typename T>
class AwaitProcess : public Process<AwaitProcess<T>>
{
public:
  AwaitProcess(
      const std::list<Future<T>>& _futures,
      Promise<std::list<Future<T>>>* _promise)
    : futures(_futures),
      promise(_promise),
      ready(0) {}

  virtual ~AwaitProcess()
  {
    delete promise;
  }

  virtual void initialize()
  {
    // Stop this nonsense if nobody cares.
    promise->future().onDiscard(defer(this, &AwaitProcess::discarded));

    typename std::list<Future<T>>::const_iterator iterator;
    for (iterator = futures.begin(); iterator != futures.end(); ++iterator) {
      (*iterator).onAny(defer(this, &AwaitProcess::waited, lambda::_1));
    }
  }

private:
  void discarded()
  {
    promise->discard();
    terminate(this);
  }

  void waited(const Future<T>& future)
  {
    CHECK(!future.isPending());

    ready += 1;
    if (ready == futures.size()) {
      promise->set(futures);
      terminate(this);
    }
  }

  const std::list<Future<T>> futures;
  Promise<std::list<Future<T>>>* promise;
  size_t ready;
};


} // namespace internal {


template <typename T>
inline Future<std::list<T>> collect(
    const std::list<Future<T>>& futures)
{
  if (futures.empty()) {
    return std::list<T>();
  }

  Promise<std::list<T>>* promise = new Promise<std::list<T>>();
  Future<std::list<T>> future = promise->future();
  spawn(new internal::CollectProcess<T>(futures, promise), true);
  return future;
}


template <typename T>
inline Future<std::list<Future<T>>> await(
    const std::list<Future<T>>& futures)
{
  if (futures.empty()) {
    return futures;
  }

  Promise<std::list<Future<T>>>* promise =
    new Promise<std::list<Future<T>>>();
  Future<std::list<Future<T>>> future = promise->future();
  spawn(new internal::AwaitProcess<T>(futures, promise), true);
  return future;
}


template <typename T1, typename T2>
Future<std::tuple<Future<T1>, Future<T2>>> await(
    const Future<T1>& future1,
    const Future<T2>& future2)
{
  Owned<Promise<Nothing>> promise1(new Promise<Nothing>());
  Owned<Promise<Nothing>> promise2(new Promise<Nothing>());

  future1.onAny([=]() { promise1->set(Nothing()); });
  future2.onAny([=]() { promise2->set(Nothing()); });

  std::list<Future<Nothing>> futures;
  futures.push_back(promise1->future());
  futures.push_back(promise2->future());

  return await(futures)
    .then([=]() { return std::make_tuple(future1, future2); });
}


template <typename T1, typename T2, typename T3>
Future<std::tuple<Future<T1>, Future<T2>, Future<T3>>> await(
    const Future<T1>& future1,
    const Future<T2>& future2,
    const Future<T3>& future3)
{
  Owned<Promise<Nothing>> promise1(new Promise<Nothing>());
  Owned<Promise<Nothing>> promise2(new Promise<Nothing>());
  Owned<Promise<Nothing>> promise3(new Promise<Nothing>());

  future1.onAny([=]() { promise1->set(Nothing()); });
  future2.onAny([=]() { promise2->set(Nothing()); });
  future3.onAny([=]() { promise3->set(Nothing()); });

  std::list<Future<Nothing>> futures;
  futures.push_back(promise1->future());
  futures.push_back(promise2->future());
  futures.push_back(promise3->future());

  return await(futures)
    .then([=]() { return std::make_tuple(future1, future2, future3); });
}

} // namespace process {

#endif // __PROCESS_COLLECT_HPP__
