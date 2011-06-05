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


void Latch::await()
{
  assert(latch != NULL);
  if (!triggered) {
    wait(latch->self());
  }
}

} // namespace process {
