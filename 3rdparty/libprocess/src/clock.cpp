#include <ev.h>

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

#include "synchronized.hpp"

using std::list;
using std::map;

namespace process {

// Event loop.
extern struct ev_loop* loop;

// Asynchronous watcher for interrupting loop to specifically deal
// with updating timers.
static ev_async async_update_timer_watcher;

// Watcher for timeouts.
static ev_timer timeouts_watcher;

// We store the timers in a map of lists indexed by the timeout of the
// timer so that we can have two timers that have the same timeout. We
// exploit that the map is SORTED!
static map<Time, list<Timer>>* timeouts = new map<Time, list<Timer>>();
static synchronizable(timeouts) = SYNCHRONIZED_INITIALIZER_RECURSIVE;

// Flag to indicate whether or to update the timer on async interrupt.
static bool update_timer = false;


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
lambda::function<void(list<Timer>&&)> callback;

} // namespace clock {


void handle_async_update_timer(struct ev_loop* loop, ev_async* _, int revents)
{
  synchronized (timeouts) {
    if (update_timer) {
      if (!timeouts->empty()) {
        // Determine when the next timer should fire.
        timeouts_watcher.repeat =
          (timeouts->begin()->first - Clock::now()).secs();

        if (timeouts_watcher.repeat <= 0) {
          // Feed the event now!
          timeouts_watcher.repeat = 0;
          ev_timer_again(loop, &timeouts_watcher);
          ev_feed_event(loop, &timeouts_watcher, EV_TIMEOUT);
        } else {
          // Don't fire the timer if the clock is paused since we
          // don't want time to advance (instead a call to
          // clock::advance() will handle the timer).
          if (Clock::paused() && timeouts_watcher.repeat > 0) {
            timeouts_watcher.repeat = 0;
          }

          ev_timer_again(loop, &timeouts_watcher);
        }
      }

      update_timer = false;
    }
  }
}


void handle_timeouts(struct ev_loop* loop, ev_timer* _, int revents)
{
  list<Timer> timers;

  synchronized (timeouts) {
    Time now = Clock::now();

    VLOG(3) << "Handling timeouts up to " << now;

    foreachkey (const Time& timeout, *timeouts) {
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

      foreach (const Timer& timer, (*timeouts)[timeout]) {
        timers.push_back(timer);
      }
    }

    // Now erase the range of timeouts that timed out.
    timeouts->erase(timeouts->begin(), timeouts->upper_bound(now));

    // Okay, so the timeout for the next timer should not have fired.
    CHECK(timeouts->empty() || (timeouts->begin()->first > now));

    // Update the timer as necessary.
    if (!timeouts->empty()) {
      // Determine when the next timer should fire.
      timeouts_watcher.repeat =
        (timeouts->begin()->first - Clock::now()).secs();

      if (timeouts_watcher.repeat <= 0) {
        // Feed the event now!
        timeouts_watcher.repeat = 0;
        ev_timer_again(loop, &timeouts_watcher);
        ev_feed_event(loop, &timeouts_watcher, EV_TIMEOUT);
      } else {
        // Don't fire the timer if the clock is paused since we don't
        // want time to advance (instead a call to Clock::advance()
        // will handle the timer).
        if (Clock::paused() && timeouts_watcher.repeat > 0) {
          timeouts_watcher.repeat = 0;
        }

        ev_timer_again(loop, &timeouts_watcher);
      }
    }

    update_timer = false; // Since we might have a queued update_timer.
  }

  clock::callback(std::move(timers));

  // Mark 'settling' as false since there are not any more timeouts
  // that will expire before the paused time and we've finished
  // executing expired timers.
  synchronized (timeouts) {
    if (clock::paused &&
        (timeouts->size() == 0 ||
         timeouts->begin()->first > clock::current)) {
      VLOG(3) << "Clock has settled";
      clock::settling = false;
    }
  }
}


void Clock::initialize(lambda::function<void(list<Timer>&&)>&& callback)
{
  // TODO(benh): Currently this function is expected to get called
  // just after initializing libev in process::initialize. But that is
  // too tightly coupled so and we really need to move libev specific
  // intialization outside of process::initialize that both
  // process::initialize and Clock::initialize can depend on (and thus
  // call).

  clock::callback = callback;

  ev_async_init(&async_update_timer_watcher, handle_async_update_timer);
  ev_async_start(loop, &async_update_timer_watcher);

  ev_timer_init(&timeouts_watcher, handle_timeouts, 0., 2100000.0);
  ev_timer_again(loop, &timeouts_watcher);
}


Time Clock::now()
{
  return now(__process__);
}


Time Clock::now(ProcessBase* process)
{
  synchronized (timeouts) {
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

  // TODO(benh): Versus ev_now()?
  double d = ev_time();
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
  synchronized (timeouts) {
    if (timeouts->size() == 0 ||
        timer.timeout().time() < timeouts->begin()->first) {
      // Need to interrupt the loop to update/set timer repeat.
      (*timeouts)[timer.timeout().time()].push_back(timer);
      update_timer = true;
      ev_async_send(loop, &async_update_timer_watcher);
    } else {
      // Timer repeat is adequate, just add the timeout.
      CHECK(timeouts->size() >= 1);
      (*timeouts)[timer.timeout().time()].push_back(timer);
    }
  }

  return timer;
}


bool Clock::cancel(const Timer& timer)
{
  bool canceled = false;
  synchronized (timeouts) {
    // Check if the timeout is still pending, and if so, erase it. In
    // addition, erase an empty list if we just removed the last
    // timeout.
    Time time = timer.timeout().time();
    if (timeouts->count(time) > 0) {
      canceled = true;
      (*timeouts)[time].remove(timer);
      if ((*timeouts)[time].empty()) {
        timeouts->erase(time);
      }
    }
  }

  return canceled;
}


void Clock::pause()
{
  process::initialize(); // To make sure the libev watchers are ready.

  synchronized (timeouts) {
    if (!clock::paused) {
      clock::initial = clock::current = now();
      clock::paused = true;
      VLOG(2) << "Clock paused at " << clock::initial;
    }
  }

  // Note that after pausing the clock an existing libev timer might
  // still fire (invoking handle_timeout), but since paused == true no
  // "time" will actually have passed, so no timer will actually fire.
}


bool Clock::paused()
{
  return clock::paused;
}


void Clock::resume()
{
  process::initialize(); // To make sure the libev watchers are ready.

  synchronized (timeouts) {
    if (clock::paused) {
      VLOG(2) << "Clock resumed at " << clock::current;
      clock::paused = false;
      clock::settling = false;
      clock::currents->clear();
      update_timer = true;
      ev_async_send(loop, &async_update_timer_watcher);
    }
  }
}


void Clock::advance(const Duration& duration)
{
  synchronized (timeouts) {
    if (clock::paused) {
      clock::advanced += duration;
      clock::current += duration;
      VLOG(2) << "Clock advanced ("  << duration << ") to " << clock::current;
      if (!update_timer) {
        update_timer = true;
        ev_async_send(loop, &async_update_timer_watcher);
      }
    }
  }
}


void Clock::advance(ProcessBase* process, const Duration& duration)
{
  synchronized (timeouts) {
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
  synchronized (timeouts) {
    if (clock::paused) {
      if (clock::current < time) {
        clock::advanced += (time - clock::current);
        clock::current = Time(time);
        VLOG(2) << "Clock updated to " << clock::current;
        if (!update_timer) {
          update_timer = true;
          ev_async_send(loop, &async_update_timer_watcher);
        }
      }
    }
  }
}


void Clock::update(ProcessBase* process, const Time& time, Update update)
{
  synchronized (timeouts) {
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
  synchronized (timeouts) {
    CHECK(clock::paused);

    if (update_timer) {
      return false;
    } else if (clock::settling) {
      VLOG(3) << "Clock still not settled";
      return false;
    } else if (timeouts->size() == 0 ||
               timeouts->begin()->first > clock::current) {
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
