#ifndef __PROCESS_TIMER_HPP__
#define __PROCESS_TIMER_HPP__

#include <stdlib.h> // For abort.

#include <process/dispatch.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

namespace process {

// Timer support! Note that we don't store a pointer to the issuing
// process (if there is one) because we can't dereference it because
// it might no longer be valid. (Instead we can use the PID to check
// if the issuing process is still valid and get a refernce to it).

class Timer; // Forward declaration.

namespace timers {

Timer create(double secs, const std::tr1::function<void(void)>& thunk);
bool cancel(const Timer& timer);

} // namespace timers {


class Timer
{
public:
  Timer() : id(0), t(0), pid(process::UPID()), thunk(&abort) {}

  bool operator == (const Timer& that) const
  {
    return id == that.id;
  }

  // Invokes this timer's thunk.
  void operator () () const
  {
    thunk();
  }

  // Returns the timeout associated with this timer.
  Timeout timeout() const
  {
    return t;
  }

  // Returns the PID of the running process when this timer was
  // created (via timers::create) or an empty PID if no process was
  // running when this timer was created.
  process::UPID creator() const
  {
    return pid;
  }

private:
  friend Timer timers::create(double, const std::tr1::function<void(void)>&);

  Timer(long _id,
        const Timeout& _t,
        const process::UPID& _pid,
        const std::tr1::function<void(void)>& _thunk)
    : id(_id), t(_t), pid(_pid), thunk(_thunk)
  {}

  uint64_t id; // Used for equality.
  Timeout t;
  process::UPID pid; // Running process when this timer was created.
  std::tr1::function<void(void)> thunk;
};


// Delay a dispatch to a process. Returns a timer which can attempted
// to be canceled if desired (but might be firing concurrently).

template <typename T>
Timer delay(double secs,
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

  return timers::create(secs, dispatch);
}


template <typename T, typename P1, typename A1>
Timer delay(double secs,
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

  return timers::create(secs, dispatch);
}


template <typename T,
          typename P1, typename P2,
          typename A1, typename A2>
Timer delay(double secs,
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

  return timers::create(secs, dispatch);
}


template <typename T,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Timer delay(double secs,
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

  return timers::create(secs, dispatch);
}

} // namespace process {

#endif // __PROCESS_TIMER_HPP__
