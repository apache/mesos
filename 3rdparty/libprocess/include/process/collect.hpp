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
#include <tuple>
#include <vector>

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
Future<std::vector<T>> collect(const std::vector<Future<T>>& futures);


// Waits on each future specified and returns the wrapping future
// typed of a tuple of values.
template <typename... Ts>
Future<std::tuple<Ts...>> collect(const Future<Ts>&... futures);


// Waits on each future in the specified set and returns the list of
// non-pending futures.
template <typename T>
Future<std::vector<Future<T>>> await(const std::vector<Future<T>>& futures);


// Waits on each future specified and returns the wrapping future
// typed of a tuple of futures.
template <typename... Ts>
Future<std::tuple<Future<Ts>...>> await(const Future<Ts>&... futures);


// Waits on the future specified and returns after the future has been
// completed or the await has been discarded. This is useful when
// wanting to "break out" of a future chain if a discard occurs but
// the underlying future has not been discarded. For example:
//
//   Future<string> foo()
//   {
//     return bar()
//       .then([](int i) {
//         return stringify(i);
//       });
//   }
//
//   Future<stringify> future = foo();
//   future.discard();
//
// In the above code we'll propagate the discard to `bar()` but might
// wait forever if `bar()` can't discard their computation. In some
// circumstances you might want to break out early and you can do that
// by using `await`, because if we discard an `await` that function
// will return even though all of the future's it is waiting on have
// not been discarded (in other words, the `await` can be properly
// discarded even if the underlying futures have not been discarded).
//
//   Future<string> foo()
//   {
//     return await(bar())
//       .recover([](const Future<Future<string>>& future) {
//         if (future.isDiscarded()) {
//           cleanup();
//         }
//         return Failure("Discarded waiting");
//       })
//       .then([](const Future<int>& future) {
//         return future
//           .then([](int i) {
//             return stringify(i);
//           });
//       });
//   }
//
//   Future<string> future = foo();
//   future.discard();
//
// Using `await` will enable you to continue execution even if `bar()`
// does not (or can not) discard their execution.
template <typename T>
Future<Future<T>> await(const Future<T>& future)
{
  return await(std::vector<Future<T>>{future})
    .then([=]() {
      return Future<Future<T>>(future);
    });
}


namespace internal {

template <typename T>
class CollectProcess : public Process<CollectProcess<T>>
{
public:
  CollectProcess(
      const std::vector<Future<T>>& _futures,
      Promise<std::vector<T>>* _promise)
    : ProcessBase(ID::generate("__collect__")),
      futures(_futures),
      promise(_promise),
      ready(0) {}

  ~CollectProcess() override
  {
    delete promise;
  }

protected:
  void initialize() override
  {
    // Stop this nonsense if nobody cares.
    promise->future().onDiscard(defer(this, &CollectProcess::discarded));

    foreach (const Future<T>& future, futures) {
      future.onAny(defer(this, &CollectProcess::waited, lambda::_1));
      future.onAbandoned(defer(this, &CollectProcess::abandoned));
    }
  }

private:
  void abandoned()
  {
    // There is no use waiting because this future will never complete
    // so terminate this process which will cause `promise` to get
    // deleted and our future to also be abandoned.
    terminate(this);
  }

  void discarded()
  {
    foreach (Future<T> future, futures) {
      future.discard();
    }

    // NOTE: we discard the promise after we set discard on each of
    // the futures so that there is a happens-before relationship that
    // can be assumed by callers.
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
        std::vector<T> values;
        values.reserve(futures.size());

        foreach (const Future<T>& future, futures) {
          values.push_back(future.get());
        }

        promise->set(std::move(values));
        terminate(this);
      }
    }
  }

  const std::vector<Future<T>> futures;
  Promise<std::vector<T>>* promise;
  size_t ready;
};


template <typename T>
class AwaitProcess : public Process<AwaitProcess<T>>
{
public:
  AwaitProcess(
      const std::vector<Future<T>>& _futures,
      Promise<std::vector<Future<T>>>* _promise)
    : ProcessBase(ID::generate("__await__")),
      futures(_futures),
      promise(_promise),
      ready(0) {}

  ~AwaitProcess() override
  {
    delete promise;
  }

  void initialize() override
  {
    // Stop this nonsense if nobody cares.
    promise->future().onDiscard(defer(this, &AwaitProcess::discarded));

    foreach (const Future<T>& future, futures) {
      future.onAny(defer(this, &AwaitProcess::waited, lambda::_1));
      future.onAbandoned(defer(this, &AwaitProcess::abandoned));
    }
  }

private:
  void abandoned()
  {
    // There is no use waiting because this future will never complete
    // so terminate this process which will cause `promise` to get
    // deleted and our future to also be abandoned.
    terminate(this);
  }

  void discarded()
  {
    foreach (Future<T> future, futures) {
      future.discard();
    }

    // NOTE: we discard the promise after we set discard on each of
    // the futures so that there is a happens-before relationship that
    // can be assumed by callers.
    promise->discard();

    terminate(this);
  }

  void waited(const Future<T>& future)
  {
    CHECK(!future.isPending());

    ready += 1;
    if (ready == futures.size()) {
      // It is safe to move futures at this point.
      promise->set(std::move(futures));
      terminate(this);
    }
  }

  std::vector<Future<T>> futures;
  Promise<std::vector<Future<T>>>* promise;
  size_t ready;
};


} // namespace internal {


template <typename T>
inline Future<std::vector<T>> collect(
    const std::vector<Future<T>>& futures)
{
  if (futures.empty()) {
    return std::vector<T>();
  }

  Promise<std::vector<T>>* promise = new Promise<std::vector<T>>();
  Future<std::vector<T>> future = promise->future();
  spawn(new internal::CollectProcess<T>(futures, promise), true);
  return future;
}


template <typename... Ts>
Future<std::tuple<Ts...>> collect(const Future<Ts>&... futures)
{
  std::vector<Future<Nothing>> wrappers = {
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
inline Future<std::vector<Future<T>>> await(
    const std::vector<Future<T>>& futures)
{
  if (futures.empty()) {
    return futures;
  }

  Promise<std::vector<Future<T>>>* promise =
    new Promise<std::vector<Future<T>>>();
  Future<std::vector<Future<T>>> future = promise->future();
  spawn(new internal::AwaitProcess<T>(futures, promise), true);
  return future;
}


template <typename... Ts>
Future<std::tuple<Future<Ts>...>> await(const Future<Ts>&... futures)
{
  std::vector<Future<Nothing>> wrappers = {
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
