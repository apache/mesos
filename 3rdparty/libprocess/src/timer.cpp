#include <process/timeout.hpp>
#include <process/timer.hpp>

#include <stout/lambda.hpp>

namespace process {

class TimerProcess : public Process<TimerProcess>
{
public:
  TimerProcess(double _secs,
               const UPID& _pid,
               lambda::function<void(ProcessBase*)>* _dispatcher)
    : secs(_secs), pid(_pid), dispatcher(_dispatcher) {}

protected:
  virtual void operator () ()
  {
    if (receive(secs) == TIMEOUT) {
      internal::dispatch(pid, dispatcher);
    } else {
      delete dispatcher;
    }
  }

private:
  const double secs;
  const UPID pid;
  lambda::function<void(ProcessBase*)>* dispatcher;
};


static void dispatch()


Timer::Timer(double secs,
             const UPID& pid,
             lambda::function<void(ProcessBase*)>* dispatcher)
{
  timer = spawn(new TimerProcess(secs, pid, dispatcher), true);
}


Timer::~Timer()
{
  // NOTE: Do not terminate the timer! Some users will simply ignore
  // saving the timer because they never want to cancel, thus
  // we can not terminate it here!
}


void Timer::cancel()
{
  timeouts::cancel(timeout);
}

} // namespace process {
