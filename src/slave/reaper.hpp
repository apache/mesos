#ifndef __REAPER_HPP__
#define __REAPER_HPP__

#include <set>

#include <process/process.hpp>


namespace mesos { namespace internal { namespace slave {

class ProcessExitedListener : public process::Process<ProcessExitedListener>
{
public:
  virtual void processExited(pid_t pid, int status) = 0;
};


class Reaper : public process::Process<Reaper>
{
public:
  Reaper();
  virtual ~Reaper();

  void addProcessExitedListener(const process::PID<ProcessExitedListener>&);

protected:
  virtual void operator () ();

private:
  std::set<process::PID<ProcessExitedListener> > listeners;
};


}}} // namespace mesos { namespace internal { namespace slave {

#endif // __REAPER_HPP__
