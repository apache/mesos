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

#include <mutex>

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
// The loop abstraction takes an optional PID `pid` and uses it as the
// execution context to run the loop. The implementation does a
// `defer` on this `pid` to "pop" the stack when it needs to
// asynchronously recurse. This also lets callers synchronize
// execution with other code dispatching and deferring using `pid`. If
// `None` is passed for `pid` then no `defer` is done and the stack
// will still "pop" but be restarted from the execution context
// wherever the blocked future is completed. This is usually very safe
// when that blocked future will be completed by the IO thread, but
// should not be used if it's completed by another process (because
// you'll block that process until the next time the loop blocks).
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
template <typename Iterate, typename Body>
Future<Nothing> loop(const Option<UPID>& pid, Iterate&& iterate, Body&& body);


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
  Loop(const Option<UPID>& pid, const Iterate& iterate, const Body& body)
    : pid(pid), iterate(iterate), body(body) {}

  Loop(const Option<UPID>& pid, Iterate&& iterate, Body&& body)
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

    // Propagating discards:
    //
    // When the caller does a discard we need to propagate it to
    // either the future returned from `iterate` or the future
    // returned from `body`. One easy way to do this would be to add
    // an `onAny` callback for every future returned from `iterate`
    // and `body`, but that would be a slow memory leak that would
    // grow over time, especially if the loop was actually
    // infinite. Instead, we capture the current future that needs to
    // be discarded within a `discard` function that we'll invoke when
    // we get a discard. Because there is a race setting the `discard`
    // function and reading it out to invoke we have to synchronize
    // access using a mutex. An alternative strategy would be to use
    // something like `atomic_load` and `atomic_store` with
    // `shared_ptr` so that we can swap the current future(s)
    // atomically.
    promise.future().onDiscard([weak_self]() {
      auto self = weak_self.lock();
      if (self) {
        // We need to make a copy of the current `discard` function so
        // that we can invoke it outside of the `synchronized` block
        // in the event that discarding invokes causes the `onAny`
        // callbacks that we have added in `run` to execute which may
        // deadlock attempting to re-acquire `mutex`!
        std::function<void()> f = []() {};
        synchronized (self->mutex) {
          f = self->discard;
        }
        f();
      }
    });

    if (pid.isSome()) {
      // Start the loop using `pid` as the execution context.
      dispatch(pid.get(), [self]() {
        self->run(self->iterate());
      });
    } else {
      run(iterate());
    }

    return promise.future();
  }

  void run(Future<T> next)
  {
    auto self = shared();

    // Reset `discard` so that we're not delaying cleanup of any
    // captured futures longer than necessary.
    //
    // TODO(benh): Use `WeakFuture` in `discard` functions instead.
    discard = []() {};

    while (next.isReady()) {
      Future<bool> condition = body(next.get());
      if (condition.isReady()) {
        if (condition.get()) {
          next = iterate();
          continue;
        } else {
          promise.set(Nothing());
          return;
        }
      } else {
        auto continuation = [self](const Future<bool>& condition) {
          if (condition.isReady()) {
            if (condition.get()) {
              self->run(self->iterate());
            } else {
              self->promise.set(Nothing());
            }
          } else if (condition.isFailed()) {
            self->promise.fail(condition.failure());
          } else if (condition.isDiscarded()) {
            self->promise.discard();
          }
        };

        if (pid.isSome()) {
          condition.onAny(defer(pid.get(), continuation));
        } else {
          condition.onAny(continuation);
        }

        if (!promise.future().hasDiscard()) {
          synchronized (mutex) {
            self->discard = [=]() mutable { condition.discard(); };
          }
        }

        // There's a race between when a discard occurs and the
        // `discard` function gets invoked and therefore we must
        // explicitly always do a discard. In addition, after a
        // discard occurs we'll need to explicitly do discards for
        // each new future that blocks.
        if (promise.future().hasDiscard()) {
          condition.discard();
        }

        return;
      }
    }

    auto continuation = [self](const Future<T>& next) {
      if (next.isReady()) {
        self->run(next);
      } else if (next.isFailed()) {
        self->promise.fail(next.failure());
      } else if (next.isDiscarded()) {
        self->promise.discard();
      }
    };

    if (pid.isSome()) {
      next.onAny(defer(pid.get(), continuation));
    } else {
      next.onAny(continuation);
    }

    if (!promise.future().hasDiscard()) {
      synchronized (mutex) {
        discard = [=]() mutable { next.discard(); };
      }
    }

    // See comment above as to why we need to explicitly discard
    // regardless of the path the if statement took above.
    if (promise.future().hasDiscard()) {
      next.discard();
    }
  }

private:
  const Option<UPID> pid;
  Iterate iterate;
  Body body;
  Promise<Nothing> promise;

  // In order to discard the loop safely we capture the future that
  // needs to be discarded within the `discard` function and reading
  // and writing that function with a mutex.
  std::mutex mutex;
  std::function<void()> discard = []() {};
};

} // namespace internal {


template <typename Iterate, typename Body>
Future<Nothing> loop(const Option<UPID>& pid, Iterate&& iterate, Body&& body)
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
