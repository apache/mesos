#ifndef __STOUT_STOPWATCH_HPP__
#define __STOUT_STOPWATCH_HPP__

#include <time.h>

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif // __MACH__

#include <sys/time.h>

#include "duration.hpp"

class Stopwatch
{
public:
  Stopwatch() : running(false) { started.tv_sec = 0; started.tv_nsec = 0; }

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

  Nanoseconds elapsed()
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
