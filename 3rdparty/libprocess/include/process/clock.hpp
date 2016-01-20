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

#ifndef __PROCESS_CLOCK_HPP__
#define __PROCESS_CLOCK_HPP__

#include <list>

#include <process/time.hpp>

// TODO(benh): We should really be including <process/timer.hpp> but
// there are currently too many circular dependencies in header files
// that would need to get moved to translation units first.

#include <stout/duration.hpp>
#include <stout/lambda.hpp>

namespace process {

// Forward declarations (to avoid circular dependencies).
class ProcessBase;
class Time;
class Timer;

/**
 * Provides timers.
 */
class Clock
{
public:
  /**
   * Initialize the clock with the specified callback that will be
   * invoked whenever a batch of timers has expired.
   */
  // TODO(benh): Introduce a "channel" or listener pattern for getting
  // the expired Timers rather than passing in a callback. This might
  // mean we don't need 'initialize' or 'shutdown'.
  static void initialize(
      lambda::function<void(const std::list<Timer>&)>&& callback);

  /**
   * Clears all timers without executing them.
   *
   * The process manager must be properly finalized before the clock is
   * finalized.  This will eliminate the need for timers to activate and
   * prevent further timers from being added after finalization.
   *
   * Also, the Clock must not be paused when finalizing.
   */
  static void finalize();

  /**
   * The current clock time for either the current process that makes
   * this call or the global clock time if not invoked from a process.
   *
   * @return This process' current clock time or the global clock time
   *         if not invoked from a process.
   */
  static Time now();

  static Time now(ProcessBase* process);

  static Timer timer(
      const Duration& duration,
      const lambda::function<void()>& thunk);

  static bool cancel(const Timer& timer);

  /**
   * Pauses the clock e.g. for testing purposes.
   */
  static void pause();

  /**
   * Check whether clock is currently running.
   */
  static bool paused();

  static void resume();

  static void advance(const Duration& duration);
  static void advance(ProcessBase* process, const Duration& duration);

  static void update(const Time& time);

  // When updating the time of a particular process you can specify
  // whether or not you want to override the existing value even if
  // you're going backwards in time! SAFE means don't update the
  // previous Clock for a process if going backwards in time, where as
  // FORCE forces this change.
  enum Update
  {
    SAFE,  /*!< Don't update Clock for a process if going backwards in time. */
    FORCE, /*!< Update Clock even if going backwards in time. */
  };

  static void update(
      ProcessBase* process,
      const Time& time,
      Update update = SAFE);

  static void order(ProcessBase* from, ProcessBase* to);

  // When the clock is paused this returns only after
  //   (1) all expired timers are executed,
  //   (2) no processes are running, and
  //   (3) no processes are ready to run.
  //
  // In other words, this function blocks synchronously until no other
  // execution on any processes will occur unless the clock is
  // advanced.
  //
  // TODO(benh): Move this function elsewhere, for instance, to a
  // top-level function in the 'process' namespace since it deals with
  // both processes and the clock.
  static void settle();

  // When the clock is paused this returns true if all timers that
  // expire before the paused time have executed, otherwise false.
  // Note that if the clock continually gets advanced concurrently
  // this function may never return true because the "paused" time
  // will continue to get pushed out farther in the future making more
  // timers candidates for execution.
  static bool settled();
};

} // namespace process {

#endif // __PROCESS_CLOCK_HPP__
