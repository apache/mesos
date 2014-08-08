#if __cplusplus >= 201103L
#include <process/c++11/deferred.hpp>
#else
#ifndef __PROCESS_DEFERRED_HPP__
#define __PROCESS_DEFERRED_HPP__

#include <tr1/functional>

#include <process/future.hpp>
#include <process/pid.hpp>

#include <stout/preprocessor.hpp>

namespace process {

// Forward declarations (removing these produces cryptic compiler
// errors even though we are just using them to declare friends).
class Executor;
template <typename _F> struct _Defer;


// Acts like a function call but runs within an asynchronous execution
// context such as an Executor or a ProcessBase (enforced because only
// an executor or the 'defer' routines are allowed to create them).
template <typename F>
struct Deferred : std::tr1::function<F>
{
private:
  // Only an Executor and the 'defer' routines can create these.
  friend class Executor;

  template <typename _F> friend struct _Defer;

  friend Deferred<void(void)> defer(
      const UPID& pid,
      const std::tr1::function<void(void)>& f);

  friend Deferred<void(void)> defer(
      const std::tr1::function<void(void)>& f);

#define TEMPLATE(Z, N, DATA)                                            \
  template <ENUM_PARAMS(N, typename A)>                                 \
  friend Deferred<void(ENUM_PARAMS(N, A))> defer(                       \
      const UPID& pid,                                                  \
      const std::tr1::function<void(ENUM_PARAMS(N, A))>& f);            \
                                                                        \
  template <ENUM_PARAMS(N, typename A)>                                 \
  friend Deferred<void(ENUM_PARAMS(N, A))> defer(                       \
      const std::tr1::function<void(ENUM_PARAMS(N, A))>& f);            \
                                                                        \
  template <typename R, ENUM_PARAMS(N, typename A)>                     \
  friend Deferred<Future<R>(ENUM_PARAMS(N, A))> defer(                  \
      const UPID& pid,                                                  \
      const std::tr1::function<Future<R>(ENUM_PARAMS(N, A))>& f);       \
                                                                        \
  template <typename R, ENUM_PARAMS(N, typename A)>                     \
  friend Deferred<Future<R>(ENUM_PARAMS(N, A))> defer(                  \
      const std::tr1::function<Future<R>(ENUM_PARAMS(N, A))>& f);

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  /*implicit*/ Deferred(const std::tr1::function<F>& f)
    : std::tr1::function<F>(f) {}
};


// The result of invoking the 'defer' routines is actually an internal
// type, effectively just a wrapper around the result of invoking
// 'std::tr1::bind'. However, we want the result of bind to be
// castable to a 'Deferred' but we don't want anyone to be able to
// create a 'Deferred' so we use a level-of-indirection via this type.
template <typename F>
struct _Defer : std::tr1::_Bind<F>
{
  template <typename _F>
  operator Deferred<_F> ()
  {
    return Deferred<_F>(std::tr1::function<_F>(*this));
  }

private:
  friend class Executor;

  template <typename T>
  friend _Defer<void(*(PID<T>, void (T::*)(void)))
                (const PID<T>&, void (T::*)(void))>
  defer(const PID<T>& pid, void (T::*method)(void));

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  friend _Defer<void(*(PID<T>,                                          \
                       void (T::*)(ENUM_PARAMS(N, P)),                  \
                       ENUM_PARAMS(N, A)))                              \
                (const PID<T>&,                                         \
                 void (T::*)(ENUM_PARAMS(N, P)),                        \
                 ENUM_PARAMS(N, P))>                                    \
  defer(const PID<T>& pid,                                              \
        void (T::*method)(ENUM_PARAMS(N, P)),                           \
        ENUM_BINARY_PARAMS(N, A, a));

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  template <typename R, typename T>
  friend _Defer<Future<R>(*(PID<T>, Future<R> (T::*)(void)))(
      const PID<T>&, Future<R> (T::*)(void))>
  defer(const PID<T>& pid, Future<R> (T::*method)(void));

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  friend _Defer<Future<R>(*(PID<T>,                                     \
                            Future<R> (T::*)(ENUM_PARAMS(N, P)),        \
                            ENUM_PARAMS(N, A)))                         \
                (const PID<T>&,                                         \
                 Future<R> (T::*)(ENUM_PARAMS(N, P)),                   \
                 ENUM_PARAMS(N, P))>                                    \
  defer(const PID<T>& pid,                                              \
        Future<R> (T::*method)(ENUM_PARAMS(N, P)),                      \
        ENUM_BINARY_PARAMS(N, A, a));

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  template <typename R, typename T>
  friend _Defer<Future<R>(*(PID<T>, R (T::*)(void)))(
      const PID<T>&, R (T::*)(void))>
  defer(const PID<T>& pid, R (T::*method)(void));

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  friend _Defer<Future<R>(*(PID<T>,                                     \
                            R (T::*)(ENUM_PARAMS(N, P)),                \
                            ENUM_PARAMS(N, A)))                         \
                (const PID<T>&,                                         \
                 R (T::*)(ENUM_PARAMS(N, P)),                           \
                 ENUM_PARAMS(N, P))>                                    \
  defer(const PID<T>& pid,                                              \
        R (T::*method)(ENUM_PARAMS(N, P)),                              \
        ENUM_BINARY_PARAMS(N, A, a));

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  /*implicit*/ _Defer(const std::tr1::_Bind<F>& b)
    : std::tr1::_Bind<F>(b) {}
};

} // namespace process {

#endif // __PROCESS_DEFERRED_HPP__
#endif // __cplusplus >= 201103L
