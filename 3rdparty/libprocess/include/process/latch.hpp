#ifndef __PROCESS_LATCH_HPP__
#define __PROCESS_LATCH_HPP__

#include <process/pid.hpp>

#include <stout/duration.hpp>

namespace process {

class Latch
{
public:
  Latch();
  virtual ~Latch();

  bool operator == (const Latch& that) const { return pid == that.pid; }
  bool operator < (const Latch& that) const { return pid < that.pid; }

  // Returns true if the latch was triggered, false if the latch had
  // already been triggered.
  bool trigger();

  // Returns true if the latch was triggered within the specified
  // duration, otherwise false.
  bool await(const Duration& duration = Seconds(-1));

private:
  // Not copyable, not assignable.
  Latch(const Latch& that);
  Latch& operator = (const Latch& that);

  bool triggered;
  UPID pid;
};

}  // namespace process {

#endif // __PROCESS_LATCH_HPP__
