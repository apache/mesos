#ifndef __PROCESS_TIMEOUT_HPP__
#define __PROCESS_TIMEOUT_HPP__

#include <process/process.hpp>

namespace process {

class Timeout
{
public:
  Timeout()
  {
    timeout = Clock::now();
  }

  Timeout(double seconds)
  {
    timeout = Clock::now() + seconds;
  }

  Timeout(const Timeout& that)
  {
    timeout = that.timeout;
  }

  Timeout& operator = (const Timeout& that)
  {
    if (this != &that) {
      timeout = that.timeout;
    }

    return *this;
  }

  Timeout& operator = (double seconds)
  {
    timeout = Clock::now() + seconds;
    return *this;
  }

  // Returns the number of seconds reamining.
  double remaining() const
  {
    double seconds = timeout - Clock::now();
    return seconds > 0 ? seconds : 0;
  }

private:
  double timeout;
};

}  // namespace process {

#endif // __PROCESS_TIMEOUT_HPP__
