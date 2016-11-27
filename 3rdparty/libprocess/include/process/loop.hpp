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

#ifndef __PROCESS_LOOP_HPP__
#define __PROCESS_LOOP_HPP__

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

namespace process {

// Provides an asynchronous "loop" abstraction. This abstraction is
// helpful for code that would have synchronously been written as a
// loop but asynchronously ends up being a recursive set of functions
// which depending on the compiler may result in a stack overflow
// (i.e., a compiler that can't do sufficient tail call optimization
// may add stack frames for each recursive call).
//
// The loop abstraction takes a PID `pid` and uses it as the execution
// context to run the loop. The implementation does a `defer` on this
// `pid` to "pop" the stack when it needs to asynchronously
// recurse. This also lets callers synchronize execution with other
// code dispatching and deferring using `pid`.
//
// The two functions passed to the loop represent the loop "iterate"
// step and the loop "body" step respectively. Each invocation of
// "iterate" returns the next value and the "body" returns whether or
// not to continue looping (as well as any other processing necessary
// of course). You can think of this synchronously as:
//
//     bool condition = true;
//     do {
//       condition = body(iterate());
//     } while (condition);
//
// Asynchronously using recursion this might look like:
//
//     Future<Nothing> loop()
//     {
//       return iterate()
//         .then([](T t) {
//            return body(t)
//             .then([](bool condition) {
//               if (condition) {
//                 return loop();
//               } else {
//                 return Nothing();
//               }
//             });
//         });
//     }
//
// And asynchronously using `pid` as the execution context:
//
//     Future<Nothing> loop()
//     {
//       return iterate()
//         .then(defer(pid, [](T t) {
//            return body(t)
//             .then(defer(pid, [](bool condition) {
//               if (condition) {
//                 return loop();
//               } else {
//                 return Nothing();
//               }
//             }));
//         }));
//     }
//
// And now what this looks like using `loop`:
//
//     loop(pid,
//          []() { return iterate(); },
//          [](T t) {
//            return body(t);
//          });
//
// TODO(benh): Provide an implementation that doesn't require a `pid`
// for situations like `io::read` and `io::write` where for
// performance reasons it could make more sense to NOT defer but
// rather just let the I/O thread handle the execution.
template <typename Iterate, typename Body>
Future<Nothing> loop(const UPID& pid, Iterate&& iterate, Body&& body);


// A helper for `loop` which creates a Process for us to provide an
// execution context for running the loop.
template <typename Iterate, typename Body>
Future<Nothing> loop(Iterate&& iterate, Body&& body)
{
  ProcessBase* process = new ProcessBase();
  return loop(
      spawn(process, true), // Have libprocess free `process`.
      std::forward<Iterate>(iterate),
      std::forward<Body>(body))
    .onAny([=]() {
      terminate(process);
      // NOTE: no need to `wait` or `delete` since the `spawn` above
      // put libprocess in control of garbage collection.
    });
}


namespace internal {

template <typename Iterate, typename Body, typename T>
class Loop : public std::enable_shared_from_this<Loop<Iterate, Body, T>>
{
public:
  Loop(const UPID& pid, const Iterate& iterate, const Body& body)
    : pid(pid), iterate(iterate), body(body) {}

  Loop(const UPID& pid, Iterate&& iterate, Body&& body)
    : pid(pid), iterate(std::move(iterate)), body(std::move(body)) {}

  std::shared_ptr<Loop> shared()
  {
    // Must fully specify `shared_from_this` because we're templated.
    return std::enable_shared_from_this<Loop>::shared_from_this();
  }

  std::weak_ptr<Loop> weak()
  {
    return std::weak_ptr<Loop>(shared());
  }

  Future<Nothing> start()
  {
    auto self = shared();
    auto weak_self = weak();

    // Make sure we propagate discarding. Note that to avoid an
    // infinite memory bloat we explicitly don't add a new `onDiscard`
    // callback for every new future that gets created from invoking
    // `iterate()` or `body()` but instead discard those futures
    // explicitly with our single callback here.
    promise.future()
      .onDiscard(defer(pid, [weak_self, this]() {
        auto self = weak_self.lock();
        if (self) {
          // NOTE: There's no race here between setting `next` or
          // `condition` and calling `discard()` on those futures
          // because we're serializing execution via `defer` on
          // `pid`. An alternative would require something like
          // `atomic_shared_ptr` or a mutex.
          next.discard();
          condition.discard();
        }
      }));

    // Start the loop using `pid` as the execution context.
    dispatch(pid, [self, this]() {
      next = discard_if_necessary<T>(iterate());
      run();
    });

    return promise.future();
  }

  // Helper for discarding a future if our promise already has a
  // discard. We need to check this for every future that gets
  // returned from `iterate` and `body` because there is a race
  // between our discard callback (that was set up in `start`) from
  // being executed and us replacing that future on the next call to
  // `iterate` and `body`. Note that we explicitly don't stop the loop
  // if our promise has a discard but rather we just propagate the
  // discard on to any futures returned from `iterate` and `body`. In
  // the event of synchronous `iterate` or `body` functions this could
  // result in an infinite loop.
  template <typename U>
  Future<U> discard_if_necessary(Future<U> future) const
  {
    if (promise.future().hasDiscard()) {
      future.discard();
    }
    return future;
  }

  void run()
  {
    auto self = shared();

    while (next.isReady()) {
      condition = discard_if_necessary<bool>(body(next.get()));
      if (condition.isReady()) {
        if (condition.get()) {
          next = discard_if_necessary<T>(iterate());
          continue;
        } else {
          promise.set(Nothing());
          return;
        }
      } else {
        condition
          .onAny(defer(pid, [self, this](const Future<bool>&) {
            if (condition.isReady()) {
              if (condition.get()) {
                next = discard_if_necessary<T>(iterate());
                run();
              } else {
                promise.set(Nothing());
              }
            } else if (condition.isFailed()) {
              promise.fail(condition.failure());
            } else if (condition.isDiscarded()) {
              promise.discard();
            }
          }));
        return;
      }
    }

    next
      .onAny(defer(pid, [self, this](const Future<T>&) {
        if (next.isReady()) {
          run();
        } else if (next.isFailed()) {
          promise.fail(next.failure());
        } else if (next.isDiscarded()) {
          promise.discard();
        }
      }));
  }

private:
  const UPID pid;
  Iterate iterate;
  Body body;
  Promise<Nothing> promise;
  Future<T> next;
  Future<bool> condition;
};

} // namespace internal {


template <typename Iterate, typename Body>
Future<Nothing> loop(const UPID& pid, Iterate&& iterate, Body&& body)
{
  using T =
    typename internal::unwrap<typename result_of<Iterate()>::type>::type;

  using Loop = internal::Loop<
    typename std::decay<Iterate>::type,
    typename std::decay<Body>::type,
    T>;

  std::shared_ptr<Loop> loop(
      new Loop(pid, std::forward<Iterate>(iterate), std::forward<Body>(body)));

  return loop->start();
}

} // namespace process {

#endif // __PROCESS_LOOP_HPP__
