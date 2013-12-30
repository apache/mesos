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
Deferred<void(void)> defer(
    const PID<T>& pid,
    void (T::*method)(void))
{
  return Deferred<void(void)>([=] () {
    dispatch(pid, method);
  });
}


template <typename T>
Deferred<void(void)> defer(
    const Process<T>& process,
    void (T::*method)(void))
{
  return defer(process.self(), method);
}


template <typename T>
Deferred<void(void)> defer(
    const Process<T>* process,
    void (T::*method)(void))
{
  return defer(process->self(), method);
}


// Due to a bug (http://gcc.gnu.org/bugzilla/show_bug.cgi?id=41933)
// with variadic templates and lambdas, we still need to do
// preprocessor expansions.
#define TEMPLATE(Z, N, DATA)                                            \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  auto defer(const PID<T>& pid,                                         \
             void (T::*method)(ENUM_PARAMS(N, P)),                      \
             ENUM_BINARY_PARAMS(N, A, a))                               \
    -> _Deferred<decltype(std::bind(std::function<void(ENUM_PARAMS(N, P))>(), ENUM_PARAMS(N, a)))> \
  {                                                                     \
    return std::bind(std::function<void(ENUM_PARAMS(N, P))>(            \
        [=] (ENUM_BINARY_PARAMS(N, P, p)) {                             \
          dispatch(pid, method, ENUM_PARAMS(N, p));                     \
        }), ENUM_PARAMS(N, a));                                         \
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
Deferred<Future<R>(void)>
defer(const PID<T>& pid, Future<R> (T::*method)(void))
{
  return Deferred<Future<R>(void)>([=] () {
    return dispatch(pid, method);
  });
}

template <typename R, typename T>
Deferred<Future<R>(void)>
defer(const Process<T>& process, Future<R> (T::*method)(void))
{
  return defer(process.self(), method);
}

template <typename R, typename T>
Deferred<Future<R>(void)>
defer(const Process<T>* process, Future<R> (T::*method)(void))
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
    -> _Deferred<decltype(std::bind(std::function<Future<R>(ENUM_PARAMS(N, P))>(), ENUM_PARAMS(N, a)))> \
  {                                                                     \
    return std::bind(std::function<Future<R>(ENUM_PARAMS(N, P))>(       \
        [=] (ENUM_BINARY_PARAMS(N, P, p)) {                             \
          return dispatch(pid, method, ENUM_PARAMS(N, p));              \
        }), ENUM_PARAMS(N, a));                                         \
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


// Finaly, definitions of defer for methods returning a value:

template <typename R, typename T>
Deferred<Future<R>(void)>
defer(const PID<T>& pid, R (T::*method)(void))
{
  return Deferred<Future<R>(void)>([=] () {
    return dispatch(pid, method);
  });
}

template <typename R, typename T>
Deferred<Future<R>(void)>
defer(const Process<T>& process, R (T::*method)(void))
{
  return defer(process.self(), method);
}

template <typename R, typename T>
Deferred<Future<R>(void)>
defer(const Process<T>* process, R (T::*method)(void))
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
    -> _Deferred<decltype(std::bind(std::function<Future<R>(ENUM_PARAMS(N, P))>(), ENUM_PARAMS(N, a)))> \
  {                                                                     \
    return std::bind(std::function<Future<R>(ENUM_PARAMS(N, P))>(       \
        [=] (ENUM_BINARY_PARAMS(N, P, p)) {                             \
          return dispatch(pid, method, ENUM_PARAMS(N, p));              \
        }), ENUM_PARAMS(N, a));                                         \
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
_Deferred<F> defer(const UPID& pid, F f)
{
  return _Deferred<F>(pid, f);
}


template <typename F>
_Deferred<F> defer(F f)
{
  if (__process__ != NULL) {
    return defer(__process__->self(), f);
  }

  return __executor__->defer(f);
}

} // namespace process {

#endif // __PROCESS_DEFER_HPP__
