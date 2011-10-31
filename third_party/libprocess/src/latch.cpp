#include <process/latch.hpp>
#include <process/process.hpp>


namespace process {

// TODO(benh): Provided an "optimized" implementation of a latch that
// is libprocess aware. That is, allow integrate "waiting" on a latch
// within libprocess such that it doesn't cost a memory allocation, a
// spawn, a message send, a wait, and two user-space context-switchs.

class LatchProcess : public Process<LatchProcess>
{
protected:
  virtual void operator () () { receive(); }
};


Latch::Latch()
{
  triggered = false;

  // Deadlock is possible if one thread is trying to delete a latch
  // but the libprocess thread(s) is trying to acquire a resource the
  // deleting thread is holding. Hence, we only save the PID for
  // triggering the latch and let the GC actually do the deleting
  // (thus no waiting is necessary, and deadlocks are avoided).
  latch = spawn(new LatchProcess(), true);
}


Latch::~Latch()
{
  post(latch, TERMINATE);
}


void Latch::trigger()
{
  if (!triggered) {
    post(latch, TERMINATE);
    triggered = true;
  }
}


bool Latch::await(double secs)
{
  if (!triggered) {
    return wait(latch, secs);
  }

  return true;
}

} // namespace process {
