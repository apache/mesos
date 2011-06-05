#ifndef __LATCH_HPP__
#define __LATCH_HPP__

#include <process.hpp>

// Need a forward declaration to break the dependency chain between
// Process, Future, and Latch.
class Process;


class Latch
{
public:
  Latch();
  virtual ~Latch();

  void trigger();
  void wait();

private:
  Latch(const Latch& that);
  Latch& operator = (const Latch& that);

  bool triggered;
  Process* process;
};

#endif // __LATCH_HPP__
