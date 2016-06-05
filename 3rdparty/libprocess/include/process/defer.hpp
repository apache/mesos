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

#ifndef __PROCESS_DEFER_HPP__
#define __PROCESS_DEFER_HPP__

#include <functional>
#include <memory>

#include <process/deferred.hpp>
#include <process/dispatch.hpp>
#include <process/executor.hpp>

#include <stout/preprocessor.hpp>

namespace process {

// The defer mechanism is very similar to the dispatch mechanism (see
// dispatch.hpp), however, rather than scheduling the method to get
// invoked, the defer mechanism returns a 'Deferred' object that when
// invoked does the underlying dispatch.

// First, definitions of defer for methods returning void:

template <typename T>
Deferred<void()> defer(const PID<T>& pid, void (T::*method)())
{
  return Deferred<void()>([=]() { dispatch(pid, method); });
}


template <typename T>
Deferred<void()> defer(const Process<T>& process, void (T::*method)())
{
  return defer(process.self(), method);
}


template <typename T>
Deferred<void()> defer(const Process<T>* process, void (T::*method)())
{
  return defer(process->self(), method);
}


// Due to a bug (http://gcc.gnu.org/bugzilla/show_bug.cgi?id=41933)
// with variadic templates and lambdas, we still need to do
// preprocessor expansions. In addition, due to a bug with clang (or
// libc++) we can't use std::bind with a std::function so we have to
// explicitly use the std::function<R(P...)>::operator() (see
// http://stackoverflow.com/questions/20097616/stdbind-to-a-stdfunction-crashes-with-clang).
#define TEMPLATE(Z, N, DATA)                                            \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  auto defer(const PID<T>& pid,                                         \
             void (T::*method)(ENUM_PARAMS(N, P)),                      \
             ENUM_BINARY_PARAMS(N, A, a))                               \
    -> _Deferred<decltype(std::bind(&std::function<void(ENUM_PARAMS(N, P))>::operator(), std::function<void(ENUM_PARAMS(N, P))>(), ENUM_PARAMS(N, a)))> /* NOLINT(whitespace/line_length) */ \
  {                                                                     \
    std::function<void(ENUM_PARAMS(N, P))> f(                           \
        [=](ENUM_BINARY_PARAMS(N, P, p)) {                              \
          dispatch(pid, method, ENUM_PARAMS(N, p));                     \
        });                                                             \
    return std::bind(&std::function<void(ENUM_PARAMS(N, P))>::operator(), std::move(f), ENUM_PARAMS(N, a)); /* NOLINT(whitespace/line_length) */ \
  }                                                                     \
                                                                        \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  auto defer(const Process<T>& process,                                 \
             void (T::*method)(ENUM_PARAMS(N, P)),                      \
             ENUM_BINARY_PARAMS(N, A, a))                               \
    -> decltype(defer(process.self(), method, ENUM_PARAMS(N, a)))       \
  {                                                                     \
    return defer(process.self(), method, ENUM_PARAMS(N, a));            \
  }                                                                     \
                                                                        \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  auto defer(const Process<T>* process,                                 \
             void (T::*method)(ENUM_PARAMS(N, P)),                      \
             ENUM_BINARY_PARAMS(N, A, a))                               \
    -> decltype(defer(process->self(), method, ENUM_PARAMS(N, a)))      \
  {                                                                     \
    return defer(process->self(), method, ENUM_PARAMS(N, a));           \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE


// Next, definitions of defer for methods returning future:

template <typename R, typename T>
Deferred<Future<R>()> defer(const PID<T>& pid, Future<R> (T::*method)())
{
  return Deferred<Future<R>()>([=]() { return dispatch(pid, method); });
}

template <typename R, typename T>
Deferred<Future<R>()> defer(const Process<T>& process, Future<R> (T::*method)())
{
  return defer(process.self(), method);
}

template <typename R, typename T>
Deferred<Future<R>()> defer(const Process<T>* process, Future<R> (T::*method)())
{
  return defer(process->self(), method);
}

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  auto defer(const PID<T>& pid,                                         \
             Future<R> (T::*method)(ENUM_PARAMS(N, P)),                 \
             ENUM_BINARY_PARAMS(N, A, a))                               \
    -> _Deferred<decltype(std::bind(&std::function<Future<R>(ENUM_PARAMS(N, P))>::operator(), std::function<Future<R>(ENUM_PARAMS(N, P))>(), ENUM_PARAMS(N, a)))> /* NOLINT(whitespace/line_length) */ \
  {                                                                     \
    std::function<Future<R>(ENUM_PARAMS(N, P))> f(                      \
        [=](ENUM_BINARY_PARAMS(N, P, p)) {                              \
          return dispatch(pid, method, ENUM_PARAMS(N, p));              \
        });                                                             \
    return std::bind(&std::function<Future<R>(ENUM_PARAMS(N, P))>::operator(), std::move(f), ENUM_PARAMS(N, a)); /* NOLINT(whitespace/line_length) */ \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  auto defer(const Process<T>& process,                                 \
             Future<R> (T::*method)(ENUM_PARAMS(N, P)),                 \
             ENUM_BINARY_PARAMS(N, A, a))                               \
    -> decltype(defer(process.self(), method, ENUM_PARAMS(N, a)))       \
  {                                                                     \
    return defer(process.self(), method, ENUM_PARAMS(N, a));            \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  auto defer(const Process<T>* process,                                 \
             Future<R> (T::*method)(ENUM_PARAMS(N, P)),                 \
             ENUM_BINARY_PARAMS(N, A, a))                               \
    -> decltype(defer(process->self(), method, ENUM_PARAMS(N, a)))      \
  {                                                                     \
    return defer(process->self(), method, ENUM_PARAMS(N, a));           \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE


// Finally, definitions of defer for methods returning a value:

template <typename R, typename T>
Deferred<Future<R>()> defer(const PID<T>& pid, R (T::*method)())
{
  return Deferred<Future<R>()>([=]() { return dispatch(pid, method); });
}

template <typename R, typename T>
Deferred<Future<R>()> defer(const Process<T>& process, R (T::*method)())
{
  return defer(process.self(), method);
}

template <typename R, typename T>
Deferred<Future<R>()> defer(const Process<T>* process, R (T::*method)())
{
  return defer(process->self(), method);
}

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  auto defer(const PID<T>& pid,                                         \
             R (T::*method)(ENUM_PARAMS(N, P)),                         \
             ENUM_BINARY_PARAMS(N, A, a))                               \
    -> _Deferred<decltype(std::bind(&std::function<Future<R>(ENUM_PARAMS(N, P))>::operator(), std::function<Future<R>(ENUM_PARAMS(N, P))>(), ENUM_PARAMS(N, a)))> /* NOLINT(whitespace/line_length) */ \
  {                                                                     \
    std::function<Future<R>(ENUM_PARAMS(N, P))> f(                      \
        [=](ENUM_BINARY_PARAMS(N, P, p)) {                              \
          return dispatch(pid, method, ENUM_PARAMS(N, p));              \
        });                                                             \
    return std::bind(&std::function<Future<R>(ENUM_PARAMS(N, P))>::operator(), std::move(f), ENUM_PARAMS(N, a)); /* NOLINT(whitespace/line_length) */ \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  auto                                                                  \
  defer(const Process<T>& process,                                      \
        R (T::*method)(ENUM_PARAMS(N, P)),                              \
        ENUM_BINARY_PARAMS(N, A, a))                                    \
    -> decltype(defer(process.self(), method, ENUM_PARAMS(N, a)))       \
  {                                                                     \
    return defer(process.self(), method, ENUM_PARAMS(N, a));            \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  auto                                                                  \
  defer(const Process<T>* process,                                      \
        R (T::*method)(ENUM_PARAMS(N, P)),                              \
        ENUM_BINARY_PARAMS(N, A, a))                                    \
    -> decltype(defer(process->self(), method, ENUM_PARAMS(N, a)))      \
  {                                                                     \
    return defer(process->self(), method, ENUM_PARAMS(N, a));           \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE


// Now we define defer calls for functors (with and without a PID):

template <typename F>
_Deferred<F> defer(const UPID& pid, F&& f)
{
  return _Deferred<F>(pid, std::forward<F>(f));
}


template <typename F>
_Deferred<F> defer(F&& f)
{
  if (__process__ != nullptr) {
    return defer(__process__->self(), std::forward<F>(f));
  }

  return __executor__->defer(std::forward<F>(f));
}

} // namespace process {

#endif // __PROCESS_DEFER_HPP__
