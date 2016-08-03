// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_COLLECT_HPP__
#define __PROCESS_COLLECT_HPP__

#include <functional>
#include <list>
#include <tuple>

#include <process/check.hpp>
#include <process/defer.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
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


// Waits on each future specified and returns the wrapping future
// typed of a tuple of values.
template <typename... Ts>
Future<std::tuple<Ts...>> collect(const Future<Ts>&... futures);


// Waits on each future in the specified set and returns the list of
// non-pending futures.
template <typename T>
Future<std::list<Future<T>>> await(const std::list<Future<T>>& futures);


// Waits on each future specified and returns the wrapping future
// typed of a tuple of futures.
template <typename... Ts>
Future<std::tuple<Future<Ts>...>> await(const Future<Ts>&... futures);


namespace internal {

template <typename T>
class CollectProcess : public Process<CollectProcess<T>>
{
public:
  CollectProcess(
      const std::list<Future<T>>& _futures,
      Promise<std::list<T>>* _promise)
    : ProcessBase(ID::generate("__collect__")),
      futures(_futures),
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

    foreach (const Future<T>& future, futures) {
      future.onAny(defer(this, &CollectProcess::waited, lambda::_1));
    }
  }

private:
  void discarded()
  {
    promise->discard();

    foreach (Future<T> future, futures) {
      future.discard();
    }

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
    : ProcessBase(ID::generate("__await__")),
      futures(_futures),
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

    foreach (const Future<T>& future, futures) {
      future.onAny(defer(this, &AwaitProcess::waited, lambda::_1));
    }
  }

private:
  void discarded()
  {
    promise->discard();

    foreach (Future<T> future, futures) {
      future.discard();
    }

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


template <typename... Ts>
Future<std::tuple<Ts...>> collect(const Future<Ts>&... futures)
{
  std::list<Future<Nothing>> wrappers = {
    futures.then([]() { return Nothing(); })...
  };

  // TODO(klueska): Unfortunately, we have to use a lambda followed
  // by a bind here because of a bug in gcc 4.8 to handle variadic
  // parameters in lambdas:
  //     https://gcc.gnu.org/bugzilla/show_bug.cgi?id=47226
  auto f = [](const Future<Ts>&... futures) {
    return std::make_tuple(futures.get()...);
  };

  return collect(wrappers)
    .then(std::bind(f, futures...));
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


template <typename... Ts>
Future<std::tuple<Future<Ts>...>> await(const Future<Ts>&... futures)
{
  std::list<Future<Nothing>> wrappers = {
    futures.then([]() { return Nothing(); })...
  };

  // TODO(klueska): Unfortunately, we have to use a lambda followed
  // by a bind here because of a bug in gcc 4.8 to handle variadic
  // parameters in lambdas:
  //     https://gcc.gnu.org/bugzilla/show_bug.cgi?id=47226
  auto f = [](const Future<Ts>&... futures) {
    return std::make_tuple(futures...);
  };

  return await(wrappers)
    .then(std::bind(f, futures...));
}

} // namespace process {

#endif // __PROCESS_COLLECT_HPP__
