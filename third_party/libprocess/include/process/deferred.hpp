#ifndef __PROCESS_DEFERRED_HPP__
#define __PROCESS_DEFERRED_HPP__

#include <tr1/functional>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/preprocessor.hpp>

namespace process {

// Acts like a function call but runs within an asynchronous execution
// context such as an Executor or a ProcessBase (since only an
// executor or the 'defer' routines are allowed to create them).
template <typename F>
struct deferred : std::tr1::function<F>
{
private:
  // Only an Executor and the 'defer' routines can create these.
  friend class Executor;

  template <typename T>
  friend deferred<void(void)> defer(
      const PID<T>& pid,
      void (T::*method)(void));

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  friend deferred<void(void)> defer(const PID<T>& pid,                  \
                                    void (T::*method)(ENUM_PARAMS(N, P)), \
                                    ENUM_BINARY_PARAMS(N, A, a));
  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  template <typename R, typename T>
  friend deferred<Future<R>(void)> defer(
      const PID<T>& pid,
      Future<R> (T::*method)(void));

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  friend deferred<Future<R>(void)> defer(                               \
      const PID<T>& pid,                                                \
      Future<R> (T::*method)(ENUM_PARAMS(N, P)),                        \
      ENUM_BINARY_PARAMS(N, A, a));
  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  template <typename R, typename T>
  friend deferred<Future<R>(void)> defer(
      const PID<T>& pid,
      R (T::*method)(void));

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  friend deferred<Future<R>(void)> defer(                               \
      const PID<T>& pid,                                                \
      R (T::*method)(ENUM_PARAMS(N, P)),                                \
      ENUM_BINARY_PARAMS(N, A, a));
  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  deferred(const std::tr1::function<F>& f) : std::tr1::function<F>(f) {}
};

} // namespace process {

#endif // __PROCESS_DEFERRED_HPP__
