#include <process/timer.hpp>

namespace process {

class TimerProcess : public Process<TimerProcess>
{
public:
  TimerProcess(double _secs,
               const UPID& _pid,
               std::tr1::function<void(ProcessBase*)>* _dispatcher)
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
  std::tr1::function<void(ProcessBase*)>* dispatcher;
};


Timer::Timer(double secs,
             const UPID& pid,
             std::tr1::function<void(ProcessBase*)>* dispatcher)
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
  terminate(timer);
}

} // namespace process {
