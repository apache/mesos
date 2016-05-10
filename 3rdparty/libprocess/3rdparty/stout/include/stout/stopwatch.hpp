// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_STOPWATCH_HPP__
#define __STOUT_STOPWATCH_HPP__

#include <stdint.h>
#include <time.h>

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif // __MACH__

#ifndef __WINDOWS__
#include <sys/time.h>
#endif

#include "duration.hpp"

class Stopwatch
{
public:
  Stopwatch()
    : running(false)
  {
    started.tv_sec = 0;
    started.tv_nsec = 0;
    stopped.tv_sec = 0;
    stopped.tv_nsec = 0;
  }

  void start()
  {
    started = now();
    running = true;
  }

  void stop()
  {
    stopped = now();
    running = false;
  }

  Nanoseconds elapsed() const
  {
    if (!running) {
      return Nanoseconds(diff(stopped, started));
    }

    return Nanoseconds(diff(now(), started));
  }

private:
  static timespec now()
  {
    timespec ts;
#ifdef __MACH__
    // OS X does not have clock_gettime, use clock_get_time.
    clock_serv_t cclock;
    mach_timespec_t mts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);
    ts.tv_sec = mts.tv_sec;
    ts.tv_nsec = mts.tv_nsec;
#elif __WINDOWS__
    const static __int64 ticks_in_second = 10000000i64;
    const static __int64 ns_in_tick = 100;

    // Interpret the filetime as a 64-bit unsigned integer to simplify the math
    // of converting to `timespec`.
    static_assert(
        sizeof(FILETIME) == sizeof(__int64),
        "stopwatch: We currently require `FILETIME` to be 64 bits in size");
    __int64 filetime_in_ticks;

    GetSystemTimeAsFileTime(reinterpret_cast<FILETIME*>(&filetime_in_ticks));

    // Conversions. `tv_sec` is elapsed seconds, and `tv_nsec` is the remainder
    // of the elapsed time, in nanoseconds.
    ts.tv_sec  = filetime_in_ticks / ticks_in_second;
    ts.tv_nsec = filetime_in_ticks % ticks_in_second * ns_in_tick;
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif // __MACH__
    return ts;
  }

  static uint64_t diff(const timespec& from, const timespec& to)
  {
    return ((from.tv_sec - to.tv_sec) * 1000000000LL)
      + (from.tv_nsec - to.tv_nsec);
  }

  bool running;
  timespec started, stopped;
};

#endif // __STOUT_STOPWATCH_HPP__
