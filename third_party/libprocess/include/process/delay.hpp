#ifndef __PROCESS_DELAY_HPP__
#define __PROCESS_DELAY_HPP__

#include <tr1/functional>

#include <process/dispatch.hpp>
#include <process/timer.hpp>

#include <stout/duration.hpp>

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
    std::tr1::bind(internal::dispatch, pid, dispatcher);

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


template <typename T, typename P1, typename A1>
Timer delay(const Duration& duration,
            const PID<T>& pid,
            void (T::*method)(P1),
            A1 a1)
{
  std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(
      new std::tr1::function<void(T*)>(
          std::tr1::bind(method, std::tr1::placeholders::_1, a1)));

  std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher(
      new std::tr1::function<void(ProcessBase*)>(
          std::tr1::bind(&internal::vdispatcher<T>,
                         std::tr1::placeholders::_1,
                         thunk)));

  std::tr1::function<void(void)> dispatch =
    std::tr1::bind(internal::dispatch, pid, dispatcher);

  return Timer::create(duration, dispatch);
}


template <typename T, typename P1, typename A1>
Timer delay(const Duration& duration,
            const Process<T>& process,
            void (T::*method)(P1),
            A1 a1)
{
  return delay(duration, process.self(), method, a1);
}


template <typename T, typename P1, typename A1>
Timer delay(const Duration& duration,
            const Process<T>* process,
            void (T::*method)(P1),
            A1 a1)
{
  return delay(duration, process->self(), method, a1);
}


template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Timer delay(const Duration& duration,
            const PID<T>& pid,
            void (T::*method)(P1, P2),
            A1 a1, A2 a2)
{
  std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(
      new std::tr1::function<void(T*)>(
          std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2)));

  std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher(
      new std::tr1::function<void(ProcessBase*)>(
          std::tr1::bind(&internal::vdispatcher<T>,
                         std::tr1::placeholders::_1,
                         thunk)));

  std::tr1::function<void(void)> dispatch =
    std::tr1::bind(internal::dispatch, pid, dispatcher);

  return Timer::create(duration, dispatch);
}


template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Timer delay(const Duration& duration,
            const Process<T>& process,
            void (T::*method)(P1, P2),
            A1 a1, A2 a2)
{
  return delay(duration, process.self(), method, a1, a2);
}


template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Timer delay(const Duration& duration,
            const Process<T>* process,
            void (T::*method)(P1, P2),
            A1 a1, A2 a2)
{
  return delay(duration, process->self(), method, a1, a2);
}


template <typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Timer delay(const Duration& duration,
            const PID<T>& pid,
            void (T::*method)(P1, P2, P3),
            A1 a1, A2 a2, A3 a3)
{
  std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(
      new std::tr1::function<void(T*)>(
          std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2, a3)));

  std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher(
      new std::tr1::function<void(ProcessBase*)>(
          std::tr1::bind(&internal::vdispatcher<T>,
                         std::tr1::placeholders::_1,
                         thunk)));

  std::tr1::function<void(void)> dispatch =
    std::tr1::bind(internal::dispatch, pid, dispatcher);

  return Timer::create(duration, dispatch);
}


template <typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Timer delay(const Duration& duration,
            const Process<T>& process,
            void (T::*method)(P1, P2, P3),
            A1 a1, A2 a2, A3 a3)
{
  return delay(duration, process.self(), method, a1, a2, a3);
}


template <typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Timer delay(const Duration& duration,
            const Process<T>* process,
            void (T::*method)(P1, P2, P3),
            A1 a1, A2 a2, A3 a3)
{
  return delay(duration, process->self(), method, a1, a2, a3);
}


template <typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Timer delay(const Duration& duration,
            const PID<T>& pid,
            void (T::*method)(P1, P2, P3, P4),
            A1 a1, A2 a2, A3 a3, A4 a4)
{
  std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(
      new std::tr1::function<void(T*)>(
          std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2, a3, a4)));

  std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher(
      new std::tr1::function<void(ProcessBase*)>(
          std::tr1::bind(&internal::vdispatcher<T>,
                         std::tr1::placeholders::_1,
                         thunk)));

  std::tr1::function<void(void)> dispatch =
    std::tr1::bind(internal::dispatch, pid, dispatcher);

  return Timer::create(duration, dispatch);
}


template <typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Timer delay(const Duration& duration,
            const Process<T>& process,
            void (T::*method)(P1, P2, P3, P4),
            A1 a1, A2 a2, A3 a3, A4 a4)
{
  return delay(duration, process.self(), method, a1, a2, a3, a4);
}


template <typename T,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Timer delay(const Duration& duration,
            const Process<T>* process,
            void (T::*method)(P1, P2, P3, P4),
            A1 a1, A2 a2, A3 a3, A4 a4)
{
  return delay(duration, process->self(), method, a1, a2, a3, a4);
}

} // namespace process {

#endif // __PROCESS_DELAY_HPP__
