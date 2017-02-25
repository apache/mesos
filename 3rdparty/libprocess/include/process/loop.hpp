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
//          []() {
//            return iterate();
//          },
//          [](T t) {
//            return body(t);
//          });
//
// One difference between the `loop` version of the "body" versus the
// other non-loop examples above is the return value is not `bool` or
// `Future<bool>` but rather `ControlFlow<V>` or
// `Future<ControlFlow<V>>`. This enables you to return values out of
// the loop via a `Break(...)`, for example:
//
//     loop(pid,
//          []() {
//            return iterate();
//          },
//          [](T t) {
//            if (finished(t)) {
//              return Break(SomeValue());
//            }
//            return Continue();
//          });
template <typename Iterate,
          typename Body,
          typename T = typename internal::unwrap<typename result_of<Iterate()>::type>::type, // NOLINT(whitespace/line_length)
          typename CF = typename internal::unwrap<typename result_of<Body(T)>::type>::type, // NOLINT(whitespace/line_length)
          typename V = typename CF::ValueType>
Future<V> loop(const Option<UPID>& pid, Iterate&& iterate, Body&& body);


// A helper for `loop` which creates a Process for us to provide an
// execution context for running the loop.
template <typename Iterate,
          typename Body,
          typename T = typename internal::unwrap<typename result_of<Iterate()>::type>::type, // NOLINT(whitespace/line_length)
          typename CF = typename internal::unwrap<typename result_of<Body(T)>::type>::type, // NOLINT(whitespace/line_length)
          typename V = typename CF::ValueType>
Future<V> loop(Iterate&& iterate, Body&& body)
{
  // Have libprocess own and free the new `ProcessBase`.
  UPID process = spawn(new ProcessBase(), true);

  return loop<Iterate, Body, T, CF, V>(
      process,
      std::forward<Iterate>(iterate),
      std::forward<Body>(body))
    .onAny([=]() {
      terminate(process);
      // NOTE: no need to `wait` or `delete` since the `spawn` above
      // put libprocess in control of garbage collection.
    });
}


// Generic "control flow" construct that is leveraged by
// implementations such as `loop`. At a high-level a `ControlFlow`
// represents some control flow statement such as `continue` or
// `break`, however, these statements can both have values or be
// value-less (i.e., these are meant to be composed "functionally" so
// the representation of `break` captures a value that "exits the
// current function" but the representation of `continue` does not).
//
// The pattern here is to define the type/representation of control
// flow statements within the `ControlFlow` class (e.g.,
// `ControlFlow::Continue` and `ControlFlow::Break`) but also provide
// "syntactic sugar" to make it easier to use at the call site (e.g.,
// the functions `Continue()` and `Break(...)`).
template <typename T>
class ControlFlow
{
public:
  using ValueType = T;

  enum class Statement
  {
    CONTINUE,
    BREAK
  };

  class Continue
  {
  public:
    Continue() = default;

    template <typename U>
    operator ControlFlow<U>() const
    {
      return ControlFlow<U>(ControlFlow<U>::Statement::CONTINUE, None());
    }
  };

  class Break
  {
  public:
    Break(T t) : t(std::move(t)) {}

    template <typename U>
    operator ControlFlow<U>() const &
    {
      return ControlFlow<U>(ControlFlow<U>::Statement::BREAK, t);
    }

    template <typename U>
    operator ControlFlow<U>() &&
    {
      return ControlFlow<U>(ControlFlow<U>::Statement::BREAK, std::move(t));
    }

  private:
    T t;
  };

  ControlFlow(Statement s, Option<T> t) : s(s), t(std::move(t)) {}

  Statement statement() const { return s; }

  T& value() & { return t.get(); }
  const T& value() const & { return t.get(); }
  T&& value() && { return t.get(); }
  const T&& value() const && { return t.get(); }

private:
  Statement s;
  Option<T> t;
};


// Provides "syntactic sugar" for creating a `ControlFlow::Continue`.
struct Continue
{
  Continue() = default;

  template <typename T>
  operator ControlFlow<T>() const
  {
    return typename ControlFlow<T>::Continue();
  }
};


// Provides "syntactic sugar" for creating a `ControlFlow::Break`.
template <typename T>
typename ControlFlow<typename std::decay<T>::type>::Break Break(T&& t)
{
  return typename ControlFlow<typename std::decay<T>::type>::Break(
      std::forward<T>(t));
}


inline ControlFlow<Nothing>::Break Break()
{
  return ControlFlow<Nothing>::Break(Nothing());
}


namespace internal {

template <typename Iterate, typename Body, typename T, typename R>
class Loop : public std::enable_shared_from_this<Loop<Iterate, Body, T, R>>
{
public:
  template <typename Iterate_, typename Body_>
  static std::shared_ptr<Loop> create(
      const Option<UPID>& pid,
      Iterate_&& iterate,
      Body_&& body)
  {
    return std::shared_ptr<Loop>(
        new Loop(
            pid,
            std::forward<Iterate_>(iterate),
            std::forward<Body_>(body)));
  }

  std::shared_ptr<Loop> shared()
  {
    // Must fully specify `shared_from_this` because we're templated.
    return std::enable_shared_from_this<Loop>::shared_from_this();
  }

  std::weak_ptr<Loop> weak()
  {
    return std::weak_ptr<Loop>(shared());
  }

  Future<R> start()
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
    synchronized (mutex) {
      discard = []() {};
    }

    while (next.isReady()) {
      Future<ControlFlow<R>> flow = body(next.get());
      if (flow.isReady()) {
        switch (flow->statement()) {
          case ControlFlow<R>::Statement::CONTINUE: {
            next = iterate();
            continue;
          }
          case ControlFlow<R>::Statement::BREAK: {
            promise.set(flow->value());
            return;
          }
        }
      } else {
        auto continuation = [self](const Future<ControlFlow<R>>& flow) {
          if (flow.isReady()) {
            switch (flow->statement()) {
              case ControlFlow<R>::Statement::CONTINUE: {
                self->run(self->iterate());
                break;
              }
              case ControlFlow<R>::Statement::BREAK: {
                self->promise.set(flow->value());
                break;
              }
            }
          } else if (flow.isFailed()) {
            self->promise.fail(flow.failure());
          } else if (flow.isDiscarded()) {
            self->promise.discard();
          }
        };

        if (pid.isSome()) {
          flow.onAny(defer(pid.get(), continuation));
        } else {
          flow.onAny(continuation);
        }

        if (!promise.future().hasDiscard()) {
          synchronized (mutex) {
            self->discard = [=]() mutable { flow.discard(); };
          }
        }

        // There's a race between when a discard occurs and the
        // `discard` function gets invoked and therefore we must
        // explicitly always do a discard. In addition, after a
        // discard occurs we'll need to explicitly do discards for
        // each new future that blocks.
        if (promise.future().hasDiscard()) {
          flow.discard();
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

protected:
  Loop(const Option<UPID>& pid, const Iterate& iterate, const Body& body)
    : pid(pid), iterate(iterate), body(body) {}

  Loop(const Option<UPID>& pid, Iterate&& iterate, Body&& body)
    : pid(pid), iterate(std::move(iterate)), body(std::move(body)) {}

private:
  const Option<UPID> pid;
  Iterate iterate;
  Body body;
  Promise<R> promise;

  // In order to discard the loop safely we capture the future that
  // needs to be discarded within the `discard` function and reading
  // and writing that function with a mutex.
  std::mutex mutex;
  std::function<void()> discard = []() {};
};

} // namespace internal {


template <typename Iterate, typename Body, typename T, typename CF, typename V>
Future<V> loop(const Option<UPID>& pid, Iterate&& iterate, Body&& body)
{
  using Loop = internal::Loop<
    typename std::decay<Iterate>::type,
    typename std::decay<Body>::type,
    T,
    V>;

  std::shared_ptr<Loop> loop = Loop::create(
      pid,
      std::forward<Iterate>(iterate),
      std::forward<Body>(body));

  return loop->start();
}

} // namespace process {

#endif // __PROCESS_LOOP_HPP__
