#include <process.hpp>

#include "latch.hpp"


namespace process {

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
    triggered = true;
    post(latch->self(), TERMINATE);
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
