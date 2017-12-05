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

#ifndef __PROCESS_DISPATCH_HPP__
#define __PROCESS_DISPATCH_HPP__

#include <functional>
#include <memory>
#include <string>

#include <process/process.hpp>

#include <stout/lambda.hpp>
#include <stout/preprocessor.hpp>
#include <stout/result_of.hpp>

namespace process {

// The dispatch mechanism enables you to "schedule" a method to get
// invoked on a process. The result of that method invocation is
// accessible via the future that is returned by the dispatch method
// (note, however, that it might not be the _same_ future as the one
// returned from the method, if the method even returns a future, see
// below). Assuming some class 'Fibonacci' has a (visible) method
// named 'compute' that takes an integer, N (and returns the Nth
// fibonacci number) you might use dispatch like so:
//
// PID<Fibonacci> pid = spawn(new Fibonacci(), true); // Use the GC.
// Future<int> f = dispatch(pid, &Fibonacci::compute, 10);
//
// Because the pid argument is "typed" we can ensure that methods are
// only invoked on processes that are actually of that type. Providing
// this mechanism for varying numbers of function types and arguments
// requires support for variadic templates, slated to be released in
// C++11. Until then, we use the Boost preprocessor macros to
// accomplish the same thing (albeit less cleanly). See below for
// those definitions.
//
// Dispatching is done via a level of indirection. The dispatch
// routine itself creates a promise that is passed as an argument to a
// partially applied 'dispatcher' function (defined below). The
// dispatcher routines get passed to the actual process via an
// internal routine called, not surprisingly, 'dispatch', defined
// below:

namespace internal {

// The internal dispatch routine schedules a function to get invoked
// within the context of the process associated with the specified pid
// (first argument), unless that process is no longer valid. Note that
// this routine does not expect anything in particular about the
// specified function (second argument). The semantics are simple: the
// function gets applied/invoked with the process as its first
// argument.
void dispatch(
    const UPID& pid,
    std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f,
    const Option<const std::type_info*>& functionType = None());


// NOTE: This struct is used by the public `dispatch(const UPID& pid, F&& f)`
// function. See comments there for the reason why we need this.
template <typename R>
struct Dispatch;


// Partial specialization for callable objects returning `void` to be dispatched
// on a process.
// NOTE: This struct is used by the public `dispatch(const UPID& pid, F&& f)`
// function. See comments there for the reason why we need this.
template <>
struct Dispatch<void>
{
  template <typename F>
  void operator()(const UPID& pid, F&& f)
  {
    std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f_(
        new lambda::CallableOnce<void(ProcessBase*)>(
            lambda::partial(
                [](typename std::decay<F>::type&& f, ProcessBase*) {
                  std::move(f)();
                },
                std::forward<F>(f),
                lambda::_1)));

    internal::dispatch(pid, std::move(f_));
  }
};


// Partial specialization for callable objects returning `Future<R>` to be
// dispatched on a process.
// NOTE: This struct is used by the public `dispatch(const UPID& pid, F&& f)`
// function. See comments there for the reason why we need this.
template <typename R>
struct Dispatch<Future<R>>
{
  template <typename F>
  Future<R> operator()(const UPID& pid, F&& f)
  {
    std::unique_ptr<Promise<R>> promise(new Promise<R>());
    Future<R> future = promise->future();

    std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f_(
        new lambda::CallableOnce<void(ProcessBase*)>(
            lambda::partial(
                [](std::unique_ptr<Promise<R>> promise,
                   typename std::decay<F>::type&& f,
                   ProcessBase*) {
                  promise->associate(std::move(f)());
                },
                std::move(promise),
                std::forward<F>(f),
                lambda::_1)));

    internal::dispatch(pid, std::move(f_));

    return future;
  }
};


// Dispatches a callable object returning `R` on a process.
// NOTE: This struct is used by the public `dispatch(const UPID& pid, F&& f)`
// function. See comments there for the reason why we need this.
template <typename R>
struct Dispatch
{
  template <typename F>
  Future<R> operator()(const UPID& pid, F&& f)
  {
    std::unique_ptr<Promise<R>> promise(new Promise<R>());
    Future<R> future = promise->future();

    std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f_(
        new lambda::CallableOnce<void(ProcessBase*)>(
            lambda::partial(
                [](std::unique_ptr<Promise<R>> promise,
                   typename std::decay<F>::type&& f,
                   ProcessBase*) {
                  promise->set(std::move(f)());
                },
                std::move(promise),
                std::forward<F>(f),
                lambda::_1)));

    internal::dispatch(pid, std::move(f_));

    return future;
  }
};

} // namespace internal {


// Okay, now for the definition of the dispatch routines
// themselves. For each routine we provide the version in C++11 using
// variadic templates so the reader can see what the Boost
// preprocessor macros are effectively providing. Using C++11 closures
// would shorten these definitions even more.
//
// First, definitions of dispatch for methods returning void:

template <typename T>
void dispatch(const PID<T>& pid, void (T::*method)())
{
  std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f(
      new lambda::CallableOnce<void(ProcessBase*)>(
          [=](ProcessBase* process) {
            assert(process != nullptr);
            T* t = dynamic_cast<T*>(process);
            assert(t != nullptr);
            (t->*method)();
          }));

  internal::dispatch(pid, std::move(f), &typeid(method));
}

template <typename T>
void dispatch(const Process<T>& process, void (T::*method)())
{
  dispatch(process.self(), method);
}

template <typename T>
void dispatch(const Process<T>* process, void (T::*method)())
{
  dispatch(process->self(), method);
}

// Due to a bug (http://gcc.gnu.org/bugzilla/show_bug.cgi?id=41933)
// with variadic templates and lambdas, we still need to do
// preprocessor expansions.

// The following assumes base names for type and variable are `A` and `a`.
#define FORWARD(Z, N, DATA) std::forward<A ## N>(a ## N)
#define MOVE(Z, N, DATA) std::move(a ## N)
#define DECL(Z, N, DATA) typename std::decay<A ## N>::type&& a ## N

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  void dispatch(                                                        \
      const PID<T>& pid,                                                \
      void (T::*method)(ENUM_PARAMS(N, P)),                             \
      ENUM_BINARY_PARAMS(N, A, &&a))                                    \
  {                                                                     \
    std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f(        \
        new lambda::CallableOnce<void(ProcessBase*)>(                   \
            lambda::partial(                                            \
                [method](ENUM(N, DECL, _), ProcessBase* process) {      \
                  assert(process != nullptr);                           \
                  T* t = dynamic_cast<T*>(process);                     \
                  assert(t != nullptr);                                 \
                  (t->*method)(ENUM(N, MOVE, _));                       \
                },                                                      \
                ENUM(N, FORWARD, _),                                    \
                lambda::_1)));                                          \
                                                                        \
    internal::dispatch(pid, std::move(f), &typeid(method));             \
  }                                                                     \
                                                                        \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  void dispatch(                                                        \
      const Process<T>& process,                                        \
      void (T::*method)(ENUM_PARAMS(N, P)),                             \
      ENUM_BINARY_PARAMS(N, A, &&a))                                    \
  {                                                                     \
    dispatch(process.self(), method, ENUM(N, FORWARD, _));              \
  }                                                                     \
                                                                        \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  void dispatch(                                                        \
      const Process<T>* process,                                        \
      void (T::*method)(ENUM_PARAMS(N, P)),                             \
      ENUM_BINARY_PARAMS(N, A, &&a))                                    \
  {                                                                     \
    dispatch(process->self(), method, ENUM(N, FORWARD, _));             \
  }

  REPEAT_FROM_TO(1, 13, TEMPLATE, _) // Args A0 -> A11.
#undef TEMPLATE


// Next, definitions of methods returning a future:

template <typename R, typename T>
Future<R> dispatch(const PID<T>& pid, Future<R> (T::*method)())
{
  std::unique_ptr<Promise<R>> promise(new Promise<R>());
  Future<R> future = promise->future();

  std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f(
      new lambda::CallableOnce<void(ProcessBase*)>(
          lambda::partial(
              [=](std::unique_ptr<Promise<R>> promise, ProcessBase* process) {
                assert(process != nullptr);
                T* t = dynamic_cast<T*>(process);
                assert(t != nullptr);
                promise->associate((t->*method)());
              },
              std::move(promise),
              lambda::_1)));

  internal::dispatch(pid, std::move(f), &typeid(method));

  return future;
}

template <typename R, typename T>
Future<R> dispatch(const Process<T>& process, Future<R> (T::*method)())
{
  return dispatch(process.self(), method);
}

template <typename R, typename T>
Future<R> dispatch(const Process<T>* process, Future<R> (T::*method)())
{
  return dispatch(process->self(), method);
}

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> dispatch(                                                   \
      const PID<T>& pid,                                                \
      Future<R> (T::*method)(ENUM_PARAMS(N, P)),                        \
      ENUM_BINARY_PARAMS(N, A, &&a))                                    \
  {                                                                     \
    std::unique_ptr<Promise<R>> promise(new Promise<R>());              \
    Future<R> future = promise->future();                               \
                                                                        \
    std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f(        \
        new lambda::CallableOnce<void(ProcessBase*)>(                   \
            lambda::partial(                                            \
                [method](std::unique_ptr<Promise<R>> promise,           \
                         ENUM(N, DECL, _),                              \
                         ProcessBase* process) {                        \
                  assert(process != nullptr);                           \
                  T* t = dynamic_cast<T*>(process);                     \
                  assert(t != nullptr);                                 \
                  promise->associate(                                   \
                      (t->*method)(ENUM(N, MOVE, _)));                  \
                },                                                      \
                std::move(promise),                                     \
                ENUM(N, FORWARD, _),                                    \
                lambda::_1)));                                          \
                                                                        \
    internal::dispatch(pid, std::move(f), &typeid(method));             \
                                                                        \
    return future;                                                      \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> dispatch(                                                   \
      const Process<T>& process,                                        \
      Future<R> (T::*method)(ENUM_PARAMS(N, P)),                        \
      ENUM_BINARY_PARAMS(N, A, &&a))                                    \
  {                                                                     \
    return dispatch(process.self(), method, ENUM(N, FORWARD, _));       \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> dispatch(                                                   \
      const Process<T>* process,                                        \
      Future<R> (T::*method)(ENUM_PARAMS(N, P)),                        \
      ENUM_BINARY_PARAMS(N, A, &&a))                                    \
  {                                                                     \
    return dispatch(process->self(), method, ENUM(N, FORWARD, _));      \
  }

  REPEAT_FROM_TO(1, 13, TEMPLATE, _) // Args A0 -> A11.
#undef TEMPLATE


// Next, definitions of methods returning a value.

template <typename R, typename T>
Future<R> dispatch(const PID<T>& pid, R (T::*method)())
{
  std::unique_ptr<Promise<R>> promise(new Promise<R>());
  Future<R> future = promise->future();

  std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f(
      new lambda::CallableOnce<void(ProcessBase*)>(
          lambda::partial(
              [=](std::unique_ptr<Promise<R>> promise, ProcessBase* process) {
                assert(process != nullptr);
                T* t = dynamic_cast<T*>(process);
                assert(t != nullptr);
                promise->set((t->*method)());
              },
              std::move(promise),
              lambda::_1)));

  internal::dispatch(pid, std::move(f), &typeid(method));

  return future;
}

template <typename R, typename T>
Future<R> dispatch(const Process<T>& process, R (T::*method)())
{
  return dispatch(process.self(), method);
}

template <typename R, typename T>
Future<R> dispatch(const Process<T>* process, R (T::*method)())
{
  return dispatch(process->self(), method);
}

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> dispatch(                                                   \
      const PID<T>& pid,                                                \
      R (T::*method)(ENUM_PARAMS(N, P)),                                \
      ENUM_BINARY_PARAMS(N, A, &&a))                                    \
  {                                                                     \
    std::unique_ptr<Promise<R>> promise(new Promise<R>());              \
    Future<R> future = promise->future();                               \
                                                                        \
    std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f(        \
        new lambda::CallableOnce<void(ProcessBase*)>(                   \
            lambda::partial(                                            \
                [method](std::unique_ptr<Promise<R>> promise,           \
                         ENUM(N, DECL, _),                              \
                         ProcessBase* process) {                        \
                  assert(process != nullptr);                           \
                  T* t = dynamic_cast<T*>(process);                     \
                  assert(t != nullptr);                                 \
                  promise->set((t->*method)(ENUM(N, MOVE, _)));         \
                },                                                      \
                std::move(promise),                                     \
                ENUM(N, FORWARD, _),                                    \
                lambda::_1)));                                          \
                                                                        \
    internal::dispatch(pid, std::move(f), &typeid(method));             \
                                                                        \
    return future;                                                      \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> dispatch(                                                   \
      const Process<T>& process,                                        \
      R (T::*method)(ENUM_PARAMS(N, P)),                                \
      ENUM_BINARY_PARAMS(N, A, &&a))                                    \
  {                                                                     \
    return dispatch(process.self(), method, ENUM(N, FORWARD, _));       \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> dispatch(                                                   \
      const Process<T>* process,                                        \
      R (T::*method)(ENUM_PARAMS(N, P)),                                \
      ENUM_BINARY_PARAMS(N, A, &&a))                                    \
  {                                                                     \
    return dispatch(process->self(), method, ENUM(N, FORWARD, _));      \
  }

  REPEAT_FROM_TO(1, 13, TEMPLATE, _) // Args A0 -> A11.
#undef TEMPLATE

#undef DECL
#undef MOVE
#undef FORWARD

// We use partial specialization of
//   - internal::Dispatch<void> vs
//   - internal::Dispatch<Future<R>> vs
//   - internal::Dispatch
// in order to determine whether R is void, Future or other types.
template <typename F, typename R = typename result_of<F()>::type>
auto dispatch(const UPID& pid, F&& f)
  -> decltype(internal::Dispatch<R>()(pid, std::forward<F>(f)))
{
  return internal::Dispatch<R>()(pid, std::forward<F>(f));
}

} // namespace process {

#endif // __PROCESS_DISPATCH_HPP__
