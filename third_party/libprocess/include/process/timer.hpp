#ifndef __PROCESS_TIMER_HPP__
#define __PROCESS_TIMER_HPP__

#include <process/dispatch.hpp>
#include <process/process.hpp>

namespace process {

// Timer support! Note that we don't store a pointer to the issuing
// process (if there is one) because we can't dereference it because
// it might no longer be valid. (Instead we can use the PID to check
// if the issuing process is still valid and get a refernce to it).

struct timer
{
  long id;
  double timeout;
  process::UPID pid;
  std::tr1::function<void(void)> thunk;
};


inline bool operator == (const timer& left, const timer& right)
{
  return left.id == right.id;
}


namespace timers {

timer create(double secs, const std::tr1::function<void(void)>& thunk);
void cancel(const timer& timer);

} // namespace timers {


// Delay a dispatch to a process. Returns a timer which can attempted
// to be canceled if desired (but might be firing concurrently).

template <typename T>
timer delay(double secs,
            const PID<T>& pid,
            void (T::*method)())
{
  std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(
      new std::tr1::function<void(T*)>(
          std::tr1::bind(method, std::tr1::placeholders::_1)));

  std::tr1::function<void(ProcessBase*)>* dispatcher =
    new std::tr1::function<void(ProcessBase*)>(
        std::tr1::bind(&internal::vdispatcher<T>,
                       std::tr1::placeholders::_1,
                       thunk));

  std::tr1::function<void(void)> dispatch =
    std::tr1::bind(internal::dispatch, pid, dispatcher);

  return timers::create(secs, dispatch);
}


template <typename T, typename P1, typename A1>
timer delay(double secs,
            const PID<T>& pid,
            void (T::*method)(P1),
            A1 a1)
{
  std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(
      new std::tr1::function<void(T*)>(
          std::tr1::bind(method, std::tr1::placeholders::_1, a1)));

  std::tr1::function<void(ProcessBase*)>* dispatcher =
    new std::tr1::function<void(ProcessBase*)>(
        std::tr1::bind(&internal::vdispatcher<T>,
                       std::tr1::placeholders::_1,
                       thunk));

  std::tr1::function<void(void)> dispatch =
    std::tr1::bind(internal::dispatch, pid, dispatcher);

  return timers::create(secs, dispatch);
}


template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
timer delay(double secs,
            const PID<T>& pid,
            void (T::*method)(P1, P2),
            A1 a1, A2 a2)
{
  std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(
      new std::tr1::function<void(T*)>(
          std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2)));

  std::tr1::function<void(ProcessBase*)>* dispatcher =
    new std::tr1::function<void(ProcessBase*)>(
        std::tr1::bind(&internal::vdispatcher<T>,
                       std::tr1::placeholders::_1,
                       thunk));

  std::tr1::function<void(void)> dispatch =
    std::tr1::bind(internal::dispatch, pid, dispatcher);

  return timers::create(secs, dispatch);
}


template <typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
timer delay(double secs,
            const PID<T>& pid,
            void (T::*method)(P1, P2, P3),
            A1 a1, A2 a2, A3 a3)
{
  std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(
      new std::tr1::function<void(T*)>(
          std::tr1::bind(method, std::tr1::placeholders::_1, a1, a2, a3)));

  std::tr1::function<void(ProcessBase*)>* dispatcher =
    new std::tr1::function<void(ProcessBase*)>(
        std::tr1::bind(&internal::vdispatcher<T>,
                       std::tr1::placeholders::_1,
                       thunk));

  std::tr1::function<void(void)> dispatch =
    std::tr1::bind(internal::dispatch, pid, dispatcher);

  return timers::create(secs, dispatch);
}

} // namespace process {

#endif // __PROCESS_TIMER_HPP__
