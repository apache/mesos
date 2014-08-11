#if __cplusplus >= 201103L
#include <process/c++11/defer.hpp>
#else
#ifndef __PROCESS_DEFER_HPP__
#define __PROCESS_DEFER_HPP__

#include <tr1/functional>

#include <process/deferred.hpp>
#include <process/dispatch.hpp>
#include <process/executor.hpp>

#include <stout/preprocessor.hpp>

namespace process {

// The defer mechanism is very similar to the dispatch mechanism (see
// dispatch.hpp), however, rather than scheduling the method to get
// invoked, the defer mechanism returns a 'Deferred' object that when
// invoked does the underlying dispatch. Similar to dispatch, we
// provide the C++11 variadic template definitions first, and then use
// Boost preprocessor macros to provide the actual definitions.


// First, definitions of defer for methods returning void:
//
// template <typename T, typename ...P>
// Deferred<void(void)> void defer(const PID<T>& pid,
//                                 void (T::*method)(P...),
//                                 P... p)
// {
//   void (*dispatch)(const PID<T>&, void (T::*)(P...), P...) =
//     &process::template dispatch<T, P...>;

//   return Deferred<void(void)>(
//       std::tr1::bind(dispatch, pid, method, std::forward<P>(p)...));
// }

template <typename T>
_Defer<void(*(PID<T>, void (T::*)(void)))
       (const PID<T>&, void (T::*)(void))>
defer(const PID<T>& pid, void (T::*method)(void))
{
  void (*dispatch)(const PID<T>&, void (T::*)(void)) =
    &process::template dispatch<T>;
  return std::tr1::bind(dispatch, pid, method);
}

template <typename T>
_Defer<void(*(PID<T>, void (T::*)(void)))
       (const PID<T>&, void (T::*)(void))>
defer(const Process<T>& process, void (T::*method)(void))
{
  return defer(process.self(), method);
}

template <typename T>
_Defer<void(*(PID<T>, void (T::*)(void)))
       (const PID<T>&, void (T::*)(void))>
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
        void (T::*method)(ENUM_PARAMS(N, P)),                           \
        ENUM_BINARY_PARAMS(N, A, a))                                    \
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
        void (T::*method)(ENUM_PARAMS(N, P)),                           \
        ENUM_BINARY_PARAMS(N, A, a))                                    \
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
        void (T::*method)(ENUM_PARAMS(N, P)),                           \
        ENUM_BINARY_PARAMS(N, A, a))                                    \
  {                                                                     \
    return defer(process->self(), method, ENUM_PARAMS(N, a));           \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE


// Next, definitions of defer for methods returning future:
//
// template <typename R, typename T, typename ...P>
// Deferred<Future<R>(void)> void defer(const PID<T>& pid,
//                                      Future<R> (T::*method)(P...),
//                                      P... p)
// {
//   Future<R> (*dispatch)(const PID<T>&, Future<R> (T::*)(P...), P...) =
//     &process::template dispatch<R, T, P...>;
//
//   return Deferred<Future<R>(void)>(
//       std::tr1::bind(dispatch, pid, method, std::forward<P>(p)...));
// }

template <typename R, typename T>
_Defer<Future<R>(*(PID<T>, Future<R> (T::*)(void)))
       (const PID<T>&, Future<R> (T::*)(void))>
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
_Defer<Future<R>(*(PID<T>, Future<R> (T::*)(void)))
       (const PID<T>&, Future<R> (T::*)(void))>
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
        Future<R> (T::*method)(ENUM_PARAMS(N, P)),                      \
        ENUM_BINARY_PARAMS(N, A, a))                                    \
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
        Future<R> (T::*method)(ENUM_PARAMS(N, P)),                      \
        ENUM_BINARY_PARAMS(N, A, a))                                    \
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
        Future<R> (T::*method)(ENUM_PARAMS(N, P)),                      \
        ENUM_BINARY_PARAMS(N, A, a))                                    \
  {                                                                     \
    return defer(process->self(), method, ENUM_PARAMS(N, a));           \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE


// Next, definitions of defer for methods returning a value:
//
// template <typename R, typename T, typename ...P>
// Deferred<Future<R>(void)> void defer(const PID<T>& pid,
//                                      R (T::*method)(P...),
//                                      P... p)
// {
//   Future<R> (*dispatch)(const PID<T>&, R (T::*)(P...), P...) =
//     &process::template dispatch<R, T, P...>;
//
//   return Deferred<Future<R>(void)>(
//       std::tr1::bind(dispatch, pid, method, std::forward<P>(p)...));
// }

template <typename R, typename T>
_Defer<Future<R>(*(PID<T>, R (T::*)(void)))
       (const PID<T>&, R (T::*)(void))>
defer(const PID<T>& pid, R (T::*method)(void))
{
  Future<R> (*dispatch)(const PID<T>&, R (T::*)(void)) =
    &process::template dispatch<R, T>;
  return std::tr1::bind(dispatch, pid, method);
}

template <typename R, typename T>
_Defer<Future<R>(*(PID<T>, R (T::*)(void)))
       (const PID<T>&, R (T::*)(void))>
defer(const Process<T>& process, R (T::*method)(void))
{
  return defer(process.self(), method);
}

template <typename R, typename T>
_Defer<Future<R>(*(PID<T>, R (T::*)(void)))
       (const PID<T>&, R (T::*)(void))>
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
        R (T::*method)(ENUM_PARAMS(N, P)),                              \
        ENUM_BINARY_PARAMS(N, A, a))                                    \
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
        R (T::*method)(ENUM_PARAMS(N, P)),                              \
        ENUM_BINARY_PARAMS(N, A, a))                                    \
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
        R (T::*method)(ENUM_PARAMS(N, P)),                              \
        ENUM_BINARY_PARAMS(N, A, a))                                    \
  {                                                                     \
    return defer(process->self(), method, ENUM_PARAMS(N, a));           \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE


namespace internal {

inline void invoker(
    ProcessBase* _,
    const std::tr1::function<void(void)>& f)
{
  f();
}

inline void dispatcher(
    const UPID& pid,
    const std::tr1::function<void(void)>& f)
{
  std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > invoker(
      new std::tr1::function<void(ProcessBase*)>(
          std::tr1::bind(&internal::invoker,
                         std::tr1::placeholders::_1,
                         f)));

  internal::dispatch(pid, invoker);
}

#define TEMPLATE(Z, N, DATA)                                            \
  template <ENUM_PARAMS(N, typename A)>                                 \
  void CAT(vinvoker, N)(                                                \
      ProcessBase* _,                                                   \
      const std::tr1::function<void(ENUM_PARAMS(N, A))>& f,             \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    f(ENUM_PARAMS(N, a));                                               \
  }                                                                     \
                                                                        \
  template <typename R, ENUM_PARAMS(N, typename A)>                     \
  void CAT(invoker, N)(                                                 \
      ProcessBase* _,                                                   \
      const std::tr1::function<Future<R>(ENUM_PARAMS(N, A))>& f,        \
      const std::tr1::shared_ptr<Promise<R> >& promise,                 \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    if (promise->future().hasDiscard()) {                               \
      promise->discard();                                               \
    } else {                                                            \
      promise->set(f(ENUM_PARAMS(N, a)));                               \
    }                                                                   \
  }                                                                     \
                                                                        \
  template <ENUM_PARAMS(N, typename A)>                                 \
  void CAT(vdispatcher, N)(                                             \
      const UPID& pid,                                                  \
      const std::tr1::function<void(ENUM_PARAMS(N, A))>& f,             \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > invoker( \
        new std::tr1::function<void(ProcessBase*)>(                     \
            std::tr1::bind(&internal::CAT(vinvoker, N)<ENUM_PARAMS(N, A)>, \
                           std::tr1::placeholders::_1,                  \
                           f,                                           \
                           ENUM_PARAMS(N, a))));                        \
                                                                        \
    internal::dispatch(pid, invoker);                                   \
  }                                                                     \
                                                                        \
  template <typename R, ENUM_PARAMS(N, typename A)>                     \
  Future<R> CAT(dispatcher, N)(                                         \
      const UPID& pid,                                                  \
      const std::tr1::function<Future<R>(ENUM_PARAMS(N, A))>& f,        \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    std::tr1::shared_ptr<Promise<R> > promise(new Promise<R>());        \
                                                                        \
    std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > invoker( \
        new std::tr1::function<void(ProcessBase*)>(                     \
            std::tr1::bind(&internal::CAT(invoker, N)<R, ENUM_PARAMS(N, A)>, \
                           std::tr1::placeholders::_1,                  \
                           f,                                           \
                           promise,                                     \
                           ENUM_PARAMS(N, a))));                        \
                                                                        \
    internal::dispatch(pid, invoker);                                   \
                                                                        \
    return promise->future();                                           \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE


  // We can't easily use 'std::tr1::_Placeholder<X>' when doing macro
  // expansion via ENUM_BINARY_PARAMS because compilers don't like it
  // when you try and concatenate '<' 'N' '>'. Thus, we typedef them.
#define TEMPLATE(Z, N, DATA)                            \
  typedef std::tr1::_Placeholder<INC(N)> _ ## N;

  REPEAT(10, TEMPLATE, _)
#undef TEMPLATE

} // namespace internal {


// Now we define defer calls for functions and bind statements.
inline Deferred<void(void)> defer(
    const UPID& pid,
    const std::tr1::function<void(void)>& f)
{
  return std::tr1::function<void(void)>(
      std::tr1::bind(&internal::dispatcher,
                     pid,
                     f));
}


// Now we define defer calls for functions and bind statements.
inline Deferred<void(void)> defer(const std::tr1::function<void(void)>& f)
{
  if (__process__ != NULL) {
    // In C++11:
    //   const UPID pid = __process__->self();
    //   return []() {
    //     internal::dispatch(pid, [](ProcessBase* _) { f(); });
    //   }
    return std::tr1::function<void(void)>(
          std::tr1::bind(&internal::dispatcher,
                         __process__->self(),
                         f));
  }

  return __executor__->defer(f);
}


#define TEMPLATE(Z, N, DATA)                                            \
  template <ENUM_PARAMS(N, typename A)>                                 \
  Deferred<void(ENUM_PARAMS(N, A))> defer(                              \
      const UPID& pid,                                                  \
      const std::tr1::function<void(ENUM_PARAMS(N, A))>& f)             \
  {                                                                     \
    return std::tr1::function<void(ENUM_PARAMS(N, A))>(                 \
        std::tr1::bind(&internal::CAT(vdispatcher, N)<ENUM_PARAMS(N, A)>, \
                       pid,                                             \
                       f,                                               \
                       ENUM_BINARY_PARAMS(N, internal::_, () INTERCEPT))); \
  }                                                                     \
                                                                        \
  template <ENUM_PARAMS(N, typename A)>                                 \
  Deferred<void(ENUM_PARAMS(N, A))> defer(                              \
      const std::tr1::function<void(ENUM_PARAMS(N, A))>& f)             \
  {                                                                     \
    if (__process__ != NULL) {                                          \
      return std::tr1::function<void(ENUM_PARAMS(N, A))>(               \
          std::tr1::bind(&internal::CAT(vdispatcher, N)<ENUM_PARAMS(N, A)>, \
                         __process__->self(),                           \
                         f,                                             \
                         ENUM_BINARY_PARAMS(N, internal::_, () INTERCEPT))); \
    }                                                                   \
                                                                        \
    return __executor__->defer(f);                                      \
  }                                                                     \
                                                                        \
  template <typename R, ENUM_PARAMS(N, typename A)>                     \
  Deferred<Future<R>(ENUM_PARAMS(N, A))> defer(                         \
      const UPID& pid,                                                  \
      const std::tr1::function<Future<R>(ENUM_PARAMS(N, A))>& f)        \
  {                                                                     \
    return std::tr1::function<Future<R>(ENUM_PARAMS(N, A))>(            \
        std::tr1::bind(&internal::CAT(dispatcher, N)<R, ENUM_PARAMS(N, A)>, \
                       pid,                                             \
                       f,                                               \
                       ENUM_BINARY_PARAMS(N, internal::_, () INTERCEPT))); \
  }                                                                     \
                                                                        \
  template <typename R, ENUM_PARAMS(N, typename A)>                     \
  Deferred<Future<R>(ENUM_PARAMS(N, A))> defer(                         \
      const std::tr1::function<Future<R>(ENUM_PARAMS(N, A))>& f)        \
  {                                                                     \
    if (__process__ != NULL) {                                          \
      return std::tr1::function<Future<R>(ENUM_PARAMS(N, A))>(          \
          std::tr1::bind(&internal::CAT(dispatcher, N)<R, ENUM_PARAMS(N, A)>, \
                         __process__->self(),                           \
                         f,                                             \
                         ENUM_BINARY_PARAMS(N, internal::_, () INTERCEPT))); \
    }                                                                   \
                                                                        \
    return __executor__->defer(f);                                      \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

} // namespace process {

#endif // __PROCESS_DEFER_HPP__
#endif // __cplusplus >= 201103L
