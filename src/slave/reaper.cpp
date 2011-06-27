#include <sys/types.h>
#include <sys/wait.h>

#include <process/dispatch.hpp>

#include "reaper.hpp"

#include "common/foreach.hpp"

using namespace process;


namespace mesos { namespace internal { namespace slave {

Reaper::Reaper() {}


Reaper::~Reaper() {}


void Reaper::addProcessExitedListener(
    const PID<ProcessExitedListener>& listener)
{
  listeners.insert(listener);
}


void Reaper::operator () ()
{
  while (true) {
    serve(1);
    if (name() == TIMEOUT) {
      // Check whether any child process has exited.
      pid_t pid;
      int status;
      if ((pid = waitpid((pid_t) -1, &status, WNOHANG)) > 0) {
        foreach (const PID<ProcessExitedListener>& listener, listeners) {
          dispatch(listener, &ProcessExitedListener::processExited,
                   pid, status);
        }
      }
    } else if (name() == TERMINATE) {
      return;
    }
  }
}

}}} // namespace mesos { namespace internal { namespace slave {
