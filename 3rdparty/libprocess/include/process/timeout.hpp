// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

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
    Time now = Clock::now();
    if (duration < Duration::zero()) {
      // If duration is negative, we need now() + duration > Duration::min() to avoid overflow.
      // Therefore, we check for now() > duration::min() - duration.
      if (now.duration() > Duration::min() - duration) {
          return Timeout(now + duration);
      }
      return Timeout(Time::epoch());
    }

    // We need now() + duration < Duration::max() to avoid overflow.
    // Therefore, we check for now() < duration::max() - duration.
    if (now.duration() < Duration::max() - duration) {
      return Timeout(now + duration);
    }

    return Timeout(Time::max());
  }

  Timeout& operator=(const Timeout& that)
  {
    if (this != &that) {
      timeout = that.timeout;
    }

    return *this;
  }

  Timeout& operator=(const Duration& duration)
  {
    timeout = Clock::now() + duration;
    return *this;
  }

  bool operator==(const Timeout& that) const
  {
    return timeout == that.timeout;
  }

  bool operator<(const Timeout& that) const
  {
    return timeout < that.timeout;
  }

  bool operator<=(const Timeout& that) const
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
