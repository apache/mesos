#include <process.hpp>

#include "latch.hpp"


Latch::Latch()
{
  triggered = false;
  process = new Process();
  Process::spawn(process);
}


Latch::~Latch()
{
  assert(process != NULL);
  Process::post(process->self(), TERMINATE);
  Process::wait(process->self());
  delete process;
}


void Latch::trigger()
{
  assert(process != NULL);
  if (!triggered) {
    triggered = true;
    Process::post(process->self(), TERMINATE);
  }
}


void Latch::wait()
{
  assert(process != NULL);
  if (!triggered) {
    Process::wait(process->self());
  }
}
