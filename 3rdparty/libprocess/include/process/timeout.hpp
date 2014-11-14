#ifndef __PROCESS_TIMEOUT_HPP__
#define __PROCESS_TIMEOUT_HPP__

#include <process/clock.hpp>
#include <process/time.hpp>

#include <stout/duration.hpp>


namespace process {

class Timeout
{
public:
  Timeout() : timeout(Clock::now()) {}

  explicit Timeout(const Time& time) : timeout(time) {}

  Timeout(const Timeout& that) : timeout(that.timeout) {}

  // Constructs a Timeout instance from a Time that is the 'duration'
  // from now.
  static Timeout in(const Duration& duration)
  {
    return Timeout(Clock::now() + duration);
  }

  Timeout& operator = (const Timeout& that)
  {
    if (this != &that) {
      timeout = that.timeout;
    }

    return *this;
  }

  Timeout& operator = (const Duration& duration)
  {
    timeout = Clock::now() + duration;
    return *this;
  }

  bool operator == (const Timeout& that) const
  {
    return timeout == that.timeout;
  }

  bool operator < (const Timeout& that) const
  {
    return timeout < that.timeout;
  }

  bool operator <= (const Timeout& that) const
  {
    return timeout <= that.timeout;
  }

  // Returns the value of the timeout as a Time object.
  Time time() const
  {
    return timeout;
  }

  // Returns the amount of time remaining.
  Duration remaining() const
  {
    Duration remaining = timeout - Clock::now();
    return remaining > Duration::zero() ? remaining : Duration::zero();
  }

  // Returns true if the timeout expired.
  bool expired() const
  {
    return timeout <= Clock::now();
  }

private:
  Time timeout;
};

}  // namespace process {

#endif // __PROCESS_TIMEOUT_HPP__
