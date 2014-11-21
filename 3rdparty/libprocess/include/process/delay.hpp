#if __cplusplus >= 201103L
#include <process/c++11/delay.hpp>
#else
#ifndef __PROCESS_DELAY_HPP__
#define __PROCESS_DELAY_HPP__

#include <process/clock.hpp>
#include <process/dispatch.hpp>
#include <process/timer.hpp>

#include <stout/duration.hpp>
#include <stout/lambda.hpp>
#include <stout/memory.hpp>
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
  memory::shared_ptr<lambda::function<void(T*)> > thunk(
      new lambda::function<void(T*)>(
          lambda::bind(method, lambda::_1)));

  memory::shared_ptr<lambda::function<void(ProcessBase*)> > dispatcher(
      new lambda::function<void(ProcessBase*)>(
          lambda::bind(&internal::vdispatcher<T>,
                       lambda::_1,
                       thunk)));

  lambda::function<void(void)> dispatch =
    lambda::bind(internal::dispatch,
                 pid,
                 dispatcher,
                 &typeid(method));

  return Clock::timer(duration, dispatch);
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
    memory::shared_ptr<lambda::function<void(T*)> > thunk(              \
        new lambda::function<void(T*)>(                                 \
            lambda::bind(method, lambda::_1, ENUM_PARAMS(N, a))));      \
                                                                        \
    memory::shared_ptr<lambda::function<void(ProcessBase*)> > dispatcher( \
        new lambda::function<void(ProcessBase*)>(                       \
            lambda::bind(&internal::vdispatcher<T>,                     \
                         lambda::_1,                                    \
                         thunk)));                                      \
                                                                        \
    lambda::function<void(void)> dispatch =                             \
      lambda::bind(internal::dispatch,                                  \
                   pid,                                                 \
                   dispatcher,                                          \
                   &typeid(method));                                    \
                                                                        \
    return Clock::timer(duration, dispatch);                            \
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
#endif // __cplusplus >= 201103L
