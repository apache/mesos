#ifndef __PROCESS_TIMER_HPP__
#define __PROCESS_TIMER_HPP__

#include <stdlib.h> // For abort.

#include <tr1/functional>

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

} // namespace process {

#endif // __PROCESS_TIMER_HPP__
