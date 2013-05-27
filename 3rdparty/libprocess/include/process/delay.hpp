#ifndef __PROCESS_DELAY_HPP__
#define __PROCESS_DELAY_HPP__

#include <tr1/functional>

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
  std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(
      new std::tr1::function<void(T*)>(
          std::tr1::bind(method, std::tr1::placeholders::_1)));

  std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher(
      new std::tr1::function<void(ProcessBase*)>(
          std::tr1::bind(&internal::vdispatcher<T>,
                         std::tr1::placeholders::_1,
                         thunk)));

  std::tr1::function<void(void)> dispatch =
    std::tr1::bind(internal::dispatch,
                   pid,
                   dispatcher,
                   internal::canonicalize(method));

  return Timer::create(duration, dispatch);
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
    std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(          \
        new std::tr1::function<void(T*)>(                               \
            std::tr1::bind(method,                                      \
                           std::tr1::placeholders::_1,                  \
                           ENUM_PARAMS(N, a))));                        \
                                                                        \
    std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher( \
        new std::tr1::function<void(ProcessBase*)>(                     \
            std::tr1::bind(&internal::vdispatcher<T>,                   \
                           std::tr1::placeholders::_1,                  \
                           thunk)));                                    \
                                                                        \
    std::tr1::function<void(void)> dispatch =                           \
      std::tr1::bind(internal::dispatch,                                \
                     pid,                                               \
                     dispatcher,                                        \
                     internal::canonicalize(method));                   \
                                                                        \
    return Timer::create(duration, dispatch);                           \
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
