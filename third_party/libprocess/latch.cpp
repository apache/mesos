#include <process.hpp>

#include "latch.hpp"


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
  latch = new LatchProcess();
  spawn(latch);
}


Latch::~Latch()
{
  assert(latch != NULL);
  post(latch->self(), TERMINATE);
  wait(latch->self());
  delete latch;
}


void Latch::trigger()
{
  assert(latch != NULL);
  if (!triggered) {
    post(latch->self(), TERMINATE);
    triggered = true;
  }
}


bool Latch::await(double secs)
{
  assert(latch != NULL);
  if (!triggered) {
    return wait(latch->self(), secs);
  }

  return true;
}

} // namespace process {
