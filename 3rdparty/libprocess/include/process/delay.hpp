#ifndef __PROCESS_DELAY_HPP__
#define __PROCESS_DELAY_HPP__

#include <process/clock.hpp>
#include <process/dispatch.hpp>
#include <process/timer.hpp>

#include <stout/duration.hpp>
#include <stout/preprocessor.hpp>

namespace process {

// The 'delay' mechanism enables you to delay a dispatch to a process
// for some specified number of seconds. Returns a Timer instance that
// can be cancelled (but it might have already executed or be
// executing concurrently).

template <typename T>
Timer delay(const Duration& duration,
            const PID<T>& pid,
            void (T::*method)())
{
  return Clock::timer(duration, [=]() {
    dispatch(pid, method);
  });
}


template <typename T>
Timer delay(const Duration& duration,
            const Process<T>& process,
            void (T::*method)())
{
  return delay(duration, process.self(), method);
}


template <typename T>
Timer delay(const Duration& duration,
            const Process<T>* process,
            void (T::*method)())
{
  return delay(duration, process->self(), method);
}


#define TEMPLATE(Z, N, DATA)                                            \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Timer delay(const Duration& duration,                                 \
              const PID<T>& pid,                                        \
              void (T::*method)(ENUM_PARAMS(N, P)),                     \
              ENUM_BINARY_PARAMS(N, A, a))                              \
  {                                                                     \
    return Clock::timer(duration, [=]() {                               \
      dispatch(pid, method, ENUM_PARAMS(N, a));                         \
    });                                                                 \
  }                                                                     \
                                                                        \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Timer delay(const Duration& duration,                                 \
              const Process<T>& process,                                \
              void (T::*method)(ENUM_PARAMS(N, P)),                     \
              ENUM_BINARY_PARAMS(N, A, a))                              \
  {                                                                     \
    return delay(duration, process.self(), method, ENUM_PARAMS(N, a));  \
  }                                                                     \
                                                                        \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Timer delay(const Duration& duration,                                 \
              const Process<T>* process,                                \
              void (T::*method)(ENUM_PARAMS(N, P)),                     \
              ENUM_BINARY_PARAMS(N, A, a))                              \
  {                                                                     \
    return delay(duration, process->self(), method, ENUM_PARAMS(N, a)); \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

} // namespace process {

#endif // __PROCESS_DELAY_HPP__
