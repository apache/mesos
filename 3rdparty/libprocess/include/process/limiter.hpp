#ifndef __PROCESS_LIMITER_HPP__
#define __PROCESS_LIMITER_HPP__

#include <deque>

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
  ~RateLimiter();

  // Returns a future that becomes ready when the permit is acquired.
  Future<Nothing> acquire();

private:
  // Not copyable, not assignable.
  RateLimiter(const RateLimiter&);
  RateLimiter& operator = (const RateLimiter&);

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
      promise->future().discard();
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
      return promise->future();
    } if (timeout.remaining() > Seconds(0)) {
      // Need to wait a bit longer, but first one in the queue.
      Promise<Nothing>* promise = new Promise<Nothing>();
      promises.push_back(promise);
      delay(timeout.remaining(), self(), &Self::_acquire);
      return promise->future();
    }

    // No need to wait!
    timeout = Seconds(1) / permitsPerSecond;
    return Nothing();
  }

private:
  // Not copyable, not assignable.
  RateLimiterProcess(const RateLimiterProcess&);
  RateLimiterProcess& operator = (const RateLimiterProcess&);

  void _acquire()
  {
    CHECK(!promises.empty());

    Promise<Nothing>* promise = promises.front();
    promises.pop_front();

    promise->set(Nothing());

    timeout = Seconds(1) / permitsPerSecond;

    // Repeat if necessary.
    if (!promises.empty()) {
      delay(timeout.remaining(), self(), &Self::_acquire);
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


inline Future<Nothing> RateLimiter::acquire()
{
  return dispatch(process, &RateLimiterProcess::acquire);
}

} // namespace process {

#endif // __PROCESS_LIMITER_HPP__
