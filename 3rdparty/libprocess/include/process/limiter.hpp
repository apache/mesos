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

#ifndef __PROCESS_LIMITER_HPP__
#define __PROCESS_LIMITER_HPP__

#include <deque>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/future.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/nothing.hpp>

namespace process {

// Forward declaration.
class RateLimiterProcess;

// Provides an abstraction that rate limits the number of "permits"
// that can be acquired over some duration.
// NOTE: Currently, each libprocess Process should use a separate
// RateLimiter instance. This is because if multiple processes share
// a RateLimiter instance, by the time a process acts on the Future
// returned by 'acquire()' another process might have acquired the
// next permit and do its rate limited operation.
class RateLimiter
{
public:
  RateLimiter(int permits, const Duration& duration);
  explicit RateLimiter(double permitsPerSecond);
  virtual ~RateLimiter();

  // Returns a future that becomes ready when the permit is acquired.
  // Discarding this future cancels this acquisition.
  virtual Future<Nothing> acquire() const;

private:
  // Not copyable, not assignable.
  RateLimiter(const RateLimiter&);
  RateLimiter& operator=(const RateLimiter&);

  RateLimiterProcess* process;
};


class RateLimiterProcess : public Process<RateLimiterProcess>
{
public:
  RateLimiterProcess(int permits, const Duration& duration)
    : ProcessBase(ID::generate("__limiter__"))
  {
    CHECK_GT(permits, 0);
    CHECK_GT(duration.secs(), 0);
    permitsPerSecond = permits / duration.secs();
  }

  explicit RateLimiterProcess(double _permitsPerSecond)
    : ProcessBase(ID::generate("__limiter__")),
      permitsPerSecond(_permitsPerSecond)
  {
    CHECK_GT(permitsPerSecond, 0);
  }

  virtual void finalize()
  {
    foreach (Promise<Nothing>* promise, promises) {
      promise->discard();
      delete promise;
    }
    promises.clear();
  }

  Future<Nothing> acquire()
  {
    if (!promises.empty()) {
      // Need to wait for others to get permits first.
      Promise<Nothing>* promise = new Promise<Nothing>();
      promises.push_back(promise);
      return promise->future()
        .onDiscard(defer(self(), &Self::discard, promise->future()));
    }

    if (timeout.remaining() > Seconds(0)) {
      // Need to wait a bit longer, but first one in the queue.
      Promise<Nothing>* promise = new Promise<Nothing>();
      promises.push_back(promise);
      delay(timeout.remaining(), self(), &Self::_acquire);
      return promise->future()
        .onDiscard(defer(self(), &Self::discard, promise->future()));
    }

    // No need to wait!
    timeout = Seconds(1) / permitsPerSecond;
    return Nothing();
  }

private:
  // Not copyable, not assignable.
  RateLimiterProcess(const RateLimiterProcess&);
  RateLimiterProcess& operator=(const RateLimiterProcess&);

  void _acquire()
  {
    CHECK(!promises.empty());

    // Keep removing the top of the queue until we find a promise
    // whose future is not discarded.
    while (!promises.empty()) {
      Promise<Nothing>* promise = promises.front();
      promises.pop_front();
      if (!promise->future().isDiscarded()) {
        promise->set(Nothing());
        delete promise;
        timeout = Seconds(1) / permitsPerSecond;
        break;
      } else {
        delete promise;
      }
    }

    // Repeat if necessary.
    if (!promises.empty()) {
      delay(timeout.remaining(), self(), &Self::_acquire);
    }
  }

  void discard(const Future<Nothing>& future)
  {
    foreach (Promise<Nothing>* promise, promises) {
      if (promise->future() == future) {
        promise->discard();
      }
    }
  }

  double permitsPerSecond;

  Timeout timeout;

  std::deque<Promise<Nothing>*> promises;
};


inline RateLimiter::RateLimiter(int permits, const Duration& duration)
{
  process = new RateLimiterProcess(permits, duration);
  spawn(process);
}


inline RateLimiter::RateLimiter(double permitsPerSecond)
{
  process = new RateLimiterProcess(permitsPerSecond);
  spawn(process);
}


inline RateLimiter::~RateLimiter()
{
  terminate(process);
  wait(process);
  delete process;
}


inline Future<Nothing> RateLimiter::acquire() const
{
  return dispatch(process, &RateLimiterProcess::acquire);
}

} // namespace process {

#endif // __PROCESS_LIMITER_HPP__
