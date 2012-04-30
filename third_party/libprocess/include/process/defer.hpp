#ifndef __PROCESS_DEFER_HPP__
#define __PROCESS_DEFER_HPP__

#include <tr1/functional>

#include <process/deferred.hpp>
#include <process/dispatch.hpp>

namespace process {

// The defer mechanism is very similar to the dispatch mechanism (see
// dispatch.hpp), however, rather than scheduling the method to get
// invoked, the defer mechanism returns a 'deferred' object that when
// invoked does the underlying dispatch. Similar to dispatch, we
// provide the C++11 variadic template definitions first, and then use
// Boost preprocessor macros to provide the actual definitions.


// The result of invoking 'defer' is actually an internal type,
// effectively just a wrapper around the result of invoking
// 'std::tr1::bind'. However, we want the result of bind to be
// castable to a 'deferred' but we don't want anyone to be able to
// create a 'deferred' so we use a level-of-indirection via this type.
template <typename F>
struct _Defer : std::tr1::_Bind<F>
{
  template <typename _F>
  operator deferred<_F> ()
  {
    return deferred<_F>(std::tr1::function<_F>(*this));
  }

private:
  template <typename T>
  friend _Defer<void(*(PID<T>, void (T::*)(void)))(
      const PID<T>&, void (T::*)(void))>
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
         void (T::*method)(ENUM_PARAMS(N, P)),                          \
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
         Future<R> (T::*method)(ENUM_PARAMS(N, P)),                     \
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
         R (T::*method)(ENUM_PARAMS(N, P)),                             \
         ENUM_BINARY_PARAMS(N, A, a));

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  _Defer(const std::tr1::_Bind<F>& b)
    : std::tr1::_Bind<F>(b) {}
};


// First, definitions of defer for methods returning void:
//
// template <typename T, typename ...P>
// deferred<void(void)> void defer(const PID<T>& pid,
//                                 void (T::*method)(P...),
//                                 P... p)
// {
//   void (*dispatch)(const PID<T>&, void (T::*)(P...), P...) =
//     &process::template dispatch<T, P...>;

//   return deferred<void(void)>(
//       std::tr1::bind(dispatch, pid, method, std::forward<P>(p)...));
// }

template <typename T>
_Defer<void(*(PID<T>, void (T::*)(void)))(
      const PID<T>&, void (T::*)(void))>
defer(const PID<T>& pid, void (T::*method)(void))
{
  void (*dispatch)(const PID<T>&, void (T::*)(void)) =
    &process::template dispatch<T>;
  return std::tr1::bind(dispatch, pid, method);
}

template <typename T>
_Defer<void(*(PID<T>, void (T::*)(void)))(
           const PID<T>&, void (T::*)(void))>
defer(const Process<T>& process, void (T::*method)(void))
{
  return defer(process.self(), method);
}

template <typename T>
_Defer<void(*(PID<T>, void (T::*)(void)))(
           const PID<T>&, void (T::*)(void))>
defer(const Process<T>* process, void (T::*method)(void))
{
  return defer(process->self(), method);
}

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  _Defer<void(*(PID<T>,                                                 \
                void (T::*)(ENUM_PARAMS(N, P)),                         \
                ENUM_PARAMS(N, A)))                                     \
         (const PID<T>&,                                                \
          void (T::*)(ENUM_PARAMS(N, P)),                               \
          ENUM_PARAMS(N, P))>                                           \
  defer(const PID<T>& pid,                                              \
         void (T::*method)(ENUM_PARAMS(N, P)),                          \
         ENUM_BINARY_PARAMS(N, A, a))                                   \
  {                                                                     \
    void (*dispatch)(const PID<T>&,                                     \
                     void (T::*)(ENUM_PARAMS(N, P)),                    \
                     ENUM_PARAMS(N, P)) =                               \
      &process::template dispatch<T, ENUM_PARAMS(N, P), ENUM_PARAMS(N, P)>; \
    return std::tr1::bind(dispatch, pid, method, ENUM_PARAMS(N, a));    \
  }                                                                     \
                                                                        \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  _Defer<void(*(PID<T>,                                                 \
                void (T::*)(ENUM_PARAMS(N, P)),                         \
                ENUM_PARAMS(N, A)))                                     \
         (const PID<T>&,                                                \
          void (T::*)(ENUM_PARAMS(N, P)),                               \
          ENUM_PARAMS(N, P))>                                           \
  defer(const Process<T>& process,                                      \
         void (T::*method)(ENUM_PARAMS(N, P)),                          \
         ENUM_BINARY_PARAMS(N, A, a))                                   \
  {                                                                     \
    return defer(process.self(), method, ENUM_PARAMS(N, a));            \
  }                                                                     \
                                                                        \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  _Defer<void(*(PID<T>,                                                 \
                void (T::*)(ENUM_PARAMS(N, P)),                         \
                ENUM_PARAMS(N, A)))                                     \
         (const PID<T>&,                                                \
          void (T::*)(ENUM_PARAMS(N, P)),                               \
          ENUM_PARAMS(N, P))>                                           \
  defer(const Process<T>* process,                                      \
         void (T::*method)(ENUM_PARAMS(N, P)),                          \
         ENUM_BINARY_PARAMS(N, A, a))                                   \
  {                                                                     \
    return defer(process->self(), method, ENUM_PARAMS(N, a));           \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE


// Next, definitions of defer for methods returning future:
//
// template <typename R, typename T, typename ...P>
// deferred<Future<R>(void)> void defer(const PID<T>& pid,
//                                      Future<R> (T::*method)(P...),
//                                      P... p)
// {
//   Future<R> (*dispatch)(const PID<T>&, Future<R> (T::*)(P...), P...) =
//     &process::template dispatch<R, T, P...>;
//
//   return deferred<Future<R>(void)>(
//       std::tr1::bind(dispatch, pid, method, std::forward<P>(p)...));
// }

template <typename R, typename T>
_Defer<Future<R>(*(PID<T>, Future<R> (T::*)(void)))(
           const PID<T>&, Future<R> (T::*)(void))>
defer(const PID<T>& pid, Future<R> (T::*method)(void))
{
  Future<R> (*dispatch)(const PID<T>&, Future<R> (T::*)(void)) =
    &process::template dispatch<R, T>;
  return std::tr1::bind(dispatch, pid, method);
}

template <typename R, typename T>
_Defer<Future<R>(*(PID<T>, Future<R> (T::*)(void)))(
           const PID<T>&, Future<R> (T::*)(void))>
defer(const Process<T>& process, Future<R> (T::*method)(void))
{
  return defer(process.self(), method);
}

template <typename R, typename T>
_Defer<Future<R>(*(PID<T>, Future<R> (T::*)(void)))(
           const PID<T>&, Future<R> (T::*)(void))>
defer(const Process<T>* process, Future<R> (T::*method)(void))
{
  return defer(process->self(), method);
}

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  _Defer<Future<R>(*(PID<T>,                                            \
                     Future<R> (T::*)(ENUM_PARAMS(N, P)),               \
                     ENUM_PARAMS(N, A)))                                \
         (const PID<T>&,                                                \
          Future<R> (T::*)(ENUM_PARAMS(N, P)),                          \
          ENUM_PARAMS(N, P))>                                           \
  defer(const PID<T>& pid,                                              \
         Future<R> (T::*method)(ENUM_PARAMS(N, P)),                     \
         ENUM_BINARY_PARAMS(N, A, a))                                   \
  {                                                                     \
    Future<R> (*dispatch)(const PID<T>&,                                \
                          Future<R> (T::*)(ENUM_PARAMS(N, P)),          \
                          ENUM_PARAMS(N, P)) =                          \
      &process::template dispatch<R, T, ENUM_PARAMS(N, P), ENUM_PARAMS(N, P)>; \
    return std::tr1::bind(dispatch, pid, method, ENUM_PARAMS(N, a));    \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  _Defer<Future<R>(*(PID<T>,                                            \
                     Future<R> (T::*)(ENUM_PARAMS(N, P)),               \
                     ENUM_PARAMS(N, A)))                                \
         (const PID<T>&,                                                \
          Future<R> (T::*)(ENUM_PARAMS(N, P)),                          \
          ENUM_PARAMS(N, P))>                                           \
  defer(const Process<T>& process,                                      \
         Future<R> (T::*method)(ENUM_PARAMS(N, P)),                     \
         ENUM_BINARY_PARAMS(N, A, a))                                   \
  {                                                                     \
    return defer(process.self(), method, ENUM_PARAMS(N, a));            \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  _Defer<Future<R>(*(PID<T>,                                            \
                     Future<R> (T::*)(ENUM_PARAMS(N, P)),               \
                     ENUM_PARAMS(N, A)))                                \
         (const PID<T>&,                                                \
          Future<R> (T::*)(ENUM_PARAMS(N, P)),                          \
          ENUM_PARAMS(N, P))>                                           \
  defer(const Process<T>* process,                                      \
         Future<R> (T::*method)(ENUM_PARAMS(N, P)),                     \
         ENUM_BINARY_PARAMS(N, A, a))                                   \
  {                                                                     \
    return defer(process->self(), method, ENUM_PARAMS(N, a));           \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE


// Next, definitions of defer for methods returning a value:
//
// template <typename R, typename T, typename ...P>
// deferred<Future<R>(void)> void defer(const PID<T>& pid,
//                                      R (T::*method)(P...),
//                                      P... p)
// {
//   Future<R> (*dispatch)(const PID<T>&, R (T::*)(P...), P...) =
//     &process::template dispatch<R, T, P...>;
//
//   return deferred<Future<R>(void)>(
//       std::tr1::bind(dispatch, pid, method, std::forward<P>(p)...));
// }

template <typename R, typename T>
_Defer<Future<R>(*(PID<T>, R (T::*)(void)))(
           const PID<T>&, R (T::*)(void))>
defer(const PID<T>& pid, R (T::*method)(void))
{
  Future<R> (*dispatch)(const PID<T>&, R (T::*)(void)) =
    &process::template dispatch<R, T>;
  return std::tr1::bind(dispatch, pid, method);
}

template <typename R, typename T>
_Defer<Future<R>(*(PID<T>, R (T::*)(void)))(
           const PID<T>&, R (T::*)(void))>
defer(const Process<T>& process, R (T::*method)(void))
{
  return defer(process.self(), method);
}

template <typename R, typename T>
_Defer<Future<R>(*(PID<T>, R (T::*)(void)))(
           const PID<T>&, R (T::*)(void))>
defer(const Process<T>* process, R (T::*method)(void))
{
  return defer(process->self(), method);
}

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  _Defer<Future<R>(*(PID<T>,                                            \
                     R (T::*)(ENUM_PARAMS(N, P)),                       \
                     ENUM_PARAMS(N, A)))                                \
         (const PID<T>&,                                                \
          R (T::*)(ENUM_PARAMS(N, P)),                                  \
          ENUM_PARAMS(N, P))>                                           \
  defer(const PID<T>& pid,                                              \
         R (T::*method)(ENUM_PARAMS(N, P)),                             \
         ENUM_BINARY_PARAMS(N, A, a))                                   \
  {                                                                     \
    Future<R> (*dispatch)(const PID<T>&,                                \
                          R (T::*)(ENUM_PARAMS(N, P)),                  \
                          ENUM_PARAMS(N, P)) =                          \
      &process::template dispatch<R, T, ENUM_PARAMS(N, P), ENUM_PARAMS(N, P)>; \
    return std::tr1::bind(dispatch, pid, method, ENUM_PARAMS(N, a));    \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  _Defer<Future<R>(*(PID<T>,                                            \
                     R (T::*)(ENUM_PARAMS(N, P)),                       \
                     ENUM_PARAMS(N, A)))                                \
         (const PID<T>&,                                                \
          R (T::*)(ENUM_PARAMS(N, P)),                                  \
          ENUM_PARAMS(N, P))>                                           \
  defer(const Process<T>& process,                                      \
         R (T::*method)(ENUM_PARAMS(N, P)),                             \
         ENUM_BINARY_PARAMS(N, A, a))                                   \
  {                                                                     \
    return defer(process.self(), method, ENUM_PARAMS(N, a));            \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  _Defer<Future<R>(*(PID<T>,                                            \
                     R (T::*)(ENUM_PARAMS(N, P)),                       \
                     ENUM_PARAMS(N, A)))                                \
         (const PID<T>&,                                                \
          R (T::*)(ENUM_PARAMS(N, P)),                                  \
          ENUM_PARAMS(N, P))>                                           \
  defer(const Process<T>* process,                                      \
         R (T::*method)(ENUM_PARAMS(N, P)),                             \
         ENUM_BINARY_PARAMS(N, A, a))                                   \
  {                                                                     \
    return defer(process->self(), method, ENUM_PARAMS(N, a));           \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

} // namespace process {

#endif // __PROCESS_DEFER_HPP__
