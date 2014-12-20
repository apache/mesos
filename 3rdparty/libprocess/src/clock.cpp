#include <glog/logging.h>

#include <list>
#include <map>

#include <process/clock.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/time.hpp>
#include <process/timeout.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include "event_loop.hpp"
#include "synchronized.hpp"

using std::list;
using std::map;

namespace process {

// We store the timers in a map of lists indexed by the timeout of the
// timer so that we can have two timers that have the same timeout. We
// exploit that the map is SORTED!
static map<Time, list<Timer>>* timers = new map<Time, list<Timer>>();
static synchronizable(timers) = SYNCHRONIZED_INITIALIZER_RECURSIVE;


// We namespace the clock related variables to keep them well
// named. In the future we'll probably want to associate a clock with
// a specific ProcessManager/SocketManager instance pair, so this will
// likely change.
namespace clock {

map<ProcessBase*, Time>* currents = new map<ProcessBase*, Time>();

// TODO(dhamon): These static non-POD instances should be replaced by pointers
// or functions.
Time initial = Time::epoch();
Time current = Time::epoch();

Duration advanced = Duration::zero();

bool paused = false;

// For supporting Clock::settled(), false if we're not currently
// settling (or we're not paused), true if we're currently attempting
// to settle (and we're paused).
bool settling = false;

// Lambda function to invoke when timers have expired.
lambda::function<void(const list<Timer>&)> callback;


// Helper for determining the duration until the next timer elapses,
// or None if no timers are pending. Note that we don't manipulate
// 'timer's directly so that it's clear from the callsite that the use
// of 'timers' is within a 'synchronized' block.
//
// TODO(benh): Create a generic 'Timer's abstraction which hides this
// and more away (i.e., all manipulations of 'timers' below).
Option<Duration> next(const map<Time, list<Timer>>& timers)
{
  if (!timers.empty()) {
    // Determine when the next "tick" should occur.
    Duration duration = (timers.begin()->first - Clock::now());

    // Force a duration of 0 seconds (i.e., fire timers now) if the
    // clock is paused and the duration is greater than 0 since we
    // want to handle timers right away.
    if (Clock::paused() && duration > Seconds(0)) {
      return Seconds(0);
    }

    return duration;
  }

  return None();
}

} // namespace clock {


void tick()
{
  list<Timer> timedout;

  synchronized (timers) {
    Time now = Clock::now();

    VLOG(3) << "Handling timers up to " << now;

    foreachkey (const Time& timeout, *timers) {
      if (timeout > now) {
        break;
      }

      VLOG(3) << "Have timeout(s) at " << timeout;

      // Need to toggle 'settling' so that we don't prematurely say
      // we're settled until after the timers are executed below,
      // outside of the critical section.
      if (clock::paused) {
        clock::settling = true;
      }

      foreach (const Timer& timer, (*timers)[timeout]) {
        timedout.push_back(timer);
      }
    }

    // Now erase the range of timers that timed out.
    timers->erase(timers->begin(), timers->upper_bound(now));

    // Okay, so the timeout for the next timer should not have fired.
    CHECK(timers->empty() || (timers->begin()->first > now));

    // Schedule another "tick" if necessary.
    Option<Duration> duration = clock::next(*timers);
    if (duration.isSome()) {
      EventLoop::delay(duration.get(), &tick);
    }
  }

  clock::callback(timedout);

  // Mark 'settling' as false since there are not any more timers
  // that will expire before the paused time and we've finished
  // executing expired timers.
  synchronized (timers) {
    if (clock::paused &&
        (timers->size() == 0 ||
         timers->begin()->first > clock::current)) {
      VLOG(3) << "Clock has settled";
      clock::settling = false;
    }
  }
}


void Clock::initialize(lambda::function<void(const list<Timer>&)>&& callback)
{
  clock::callback = callback;
}


Time Clock::now()
{
  return now(__process__);
}


Time Clock::now(ProcessBase* process)
{
  synchronized (timers) {
    if (Clock::paused()) {
      if (process != NULL) {
        if (clock::currents->count(process) != 0) {
          return (*clock::currents)[process];
        } else {
          return (*clock::currents)[process] = clock::initial;
        }
      } else {
        return clock::current;
      }
    }
  }

  double d = EventLoop::time();
  Try<Time> time = Time::create(d); // Compensates for clock::advanced.

  // TODO(xujyan): Move CHECK_SOME to libprocess and add CHECK_SOME
  // here.
  if (time.isError()) {
    LOG(FATAL) << "Failed to create a Time from " << d << ": "
               << time.error();
  }
  return time.get();
}


Timer Clock::timer(
    const Duration& duration,
    const lambda::function<void(void)>& thunk)
{
  static uint64_t id = 1; // Start at 1 since Timer() instances use id 0.

  // Assumes Clock::now() does Clock::now(__process__).
  Timeout timeout = Timeout::in(duration);

  UPID pid = __process__ != NULL ? __process__->self() : UPID();

  Timer timer(__sync_fetch_and_add(&id, 1), timeout, pid, thunk);

  VLOG(3) << "Created a timer for " << pid << " in " << stringify(duration)
          << " in the future (" << timeout.time() << ")";

  // Add the timer.
  synchronized (timers) {
    if (timers->size() == 0 ||
        timer.timeout().time() < timers->begin()->first) {
      // Need to interrupt the loop to update/set timer repeat.

      (*timers)[timer.timeout().time()].push_back(timer);

      // Schedule another "tick" if necessary.
      Option<Duration> duration = clock::next(*timers);
      if (duration.isSome()) {
        EventLoop::delay(duration.get(), &tick);
      }
    } else {
      // Timer repeat is adequate, just add the timeout.
      CHECK(timers->size() >= 1);
      (*timers)[timer.timeout().time()].push_back(timer);
    }
  }

  return timer;
}


bool Clock::cancel(const Timer& timer)
{
  bool canceled = false;
  synchronized (timers) {
    // Check if the timeout is still pending, and if so, erase it. In
    // addition, erase an empty list if we just removed the last
    // timeout.
    Time time = timer.timeout().time();
    if (timers->count(time) > 0) {
      canceled = true;
      (*timers)[time].remove(timer);
      if ((*timers)[time].empty()) {
        timers->erase(time);
      }
    }
  }

  return canceled;
}


void Clock::pause()
{
  process::initialize(); // To make sure the event loop is ready.

  synchronized (timers) {
    if (!clock::paused) {
      clock::initial = clock::current = now();
      clock::paused = true;
      VLOG(2) << "Clock paused at " << clock::initial;
    }
  }

  // Note that after pausing the clock an existing event loop delay
  // might still fire (invoking tick), but since paused == true no
  // "time" will actually have passed, so no timer will actually fire.
}


bool Clock::paused()
{
  return clock::paused;
}


void Clock::resume()
{
  process::initialize(); // To make sure the event loop is ready.

  synchronized (timers) {
    if (clock::paused) {
      VLOG(2) << "Clock resumed at " << clock::current;

      clock::paused = false;
      clock::settling = false;
      clock::currents->clear();

      // Schedule another "tick" if necessary.
      Option<Duration> duration = clock::next(*timers);
      if (duration.isSome()) {
        EventLoop::delay(duration.get(), &tick);
      }
    }
  }
}


void Clock::advance(const Duration& duration)
{
  synchronized (timers) {
    if (clock::paused) {
      clock::advanced += duration;
      clock::current += duration;

      VLOG(2) << "Clock advanced ("  << duration << ") to " << clock::current;

      // Schedule another "tick" if necessary.
      Option<Duration> duration = clock::next(*timers);
      if (duration.isSome()) {
        EventLoop::delay(duration.get(), &tick);
      }
    }
  }
}


void Clock::advance(ProcessBase* process, const Duration& duration)
{
  synchronized (timers) {
    if (clock::paused) {
      Time current = now(process);
      current += duration;
      (*clock::currents)[process] = current;
      VLOG(2) << "Clock of " << process->self() << " advanced (" << duration
              << ") to " << current;
    }
  }
}


void Clock::update(const Time& time)
{
  synchronized (timers) {
    if (clock::paused) {
      if (clock::current < time) {
        clock::advanced += (time - clock::current);
        clock::current = Time(time);
        VLOG(2) << "Clock updated to " << clock::current;

        // Schedule another "tick" if necessary.
        Option<Duration> duration = clock::next(*timers);
        if (duration.isSome()) {
          EventLoop::delay(duration.get(), &tick);
        }
      }
    }
  }
}


void Clock::update(ProcessBase* process, const Time& time, Update update)
{
  synchronized (timers) {
    if (clock::paused) {
      if (now(process) < time || update == Clock::FORCE) {
        VLOG(2) << "Clock of " << process->self() << " updated to " << time;
        (*clock::currents)[process] = Time(time);
      }
    }
  }
}


void Clock::order(ProcessBase* from, ProcessBase* to)
{
  VLOG(2) << "Clock of " << to->self() << " being updated to " << from->self();
  update(to, now(from));
}


bool Clock::settled()
{
  synchronized (timers) {
    CHECK(clock::paused);

    if (clock::settling) {
      VLOG(3) << "Clock still not settled";
      return false;
    } else if (timers->size() == 0 ||
               timers->begin()->first > clock::current) {
      VLOG(3) << "Clock is settled";
      return true;
    }

    VLOG(3) << "Clock is not settled";
    return false;
  }

  UNREACHABLE();
}


// TODO(benh): Introduce a Clock::time(seconds) that replaces this
// function for getting a 'Time' value.
Try<Time> Time::create(double seconds)
{
  Try<Duration> duration = Duration::create(seconds);
  if (duration.isSome()) {
    // In production code, clock::advanced will always be zero!
    return Time(duration.get() + clock::advanced);
  } else {
    return Error("Argument too large for Time: " + duration.error());
  }
}

} // namespace process {
