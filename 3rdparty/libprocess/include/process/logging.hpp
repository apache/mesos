#ifndef __PROCESS_LOGGING_HPP__
#define __PROCESS_LOGGING_HPP__

#include <glog/logging.h>

#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/duration.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

namespace process {

class Logging : public Process<Logging>
{
public:
  Logging()
    : ProcessBase("logging"),
      original(FLAGS_v)
  {
    // Make sure all reads/writes can be done atomically (i.e., to
    // make sure VLOG(*) statements don't read partial writes).
    // TODO(benh): Use "atomics" primitives for doing reads/writes of
    // FLAGS_v anyway to account for proper memory barriers.
    CHECK(sizeof(FLAGS_v) == sizeof(int32_t));
  }

  virtual ~Logging() {}

protected:
  virtual void initialize()
  {
    route("/toggle", TOGGLE_HELP, &This::toggle);
  }

private:
  Future<http::Response> toggle(const http::Request& request)
  {
    Option<std::string> level = request.query.get("level");
    Option<std::string> duration = request.query.get("duration");

    if (level.isNone() && duration.isNone()) {
      return http::OK(stringify(FLAGS_v) + "\n");
    }

    if (level.isSome() && duration.isNone()) {
      return http::BadRequest("Expecting 'duration=value' in query.\n");
    } else if (level.isNone() && duration.isSome()) {
      return http::BadRequest("Expecting 'level=value' in query.\n");
    }

    Try<int> v = numify<int>(level.get());

    if (v.isError()) {
      return http::BadRequest(v.error() + ".\n");
    }

    if (v.get() < 0) {
      return http::BadRequest(
          "Invalid level '" + stringify(v.get()) + "'.\n");
    } else if (v.get() < original) {
      return http::BadRequest(
          "'" + stringify(v.get()) + "' < original level.\n");
    }

    Try<Duration> d = Duration::parse(duration.get());

    if (d.isError()) {
      return http::BadRequest(d.error() + ".\n");
    }

    // Set the logging level.
    set(v.get());

    // Start a revert timer (if necessary).
    if (v.get() != original) {
      timeout = d.get();
      delay(timeout.remaining(), this, &This::revert);
    }

    return http::OK();
  }

  void set(int v)
  {
    if (FLAGS_v != v) {
      VLOG(FLAGS_v) << "Setting verbose logging level to " << v;
      FLAGS_v = v;
      __sync_synchronize(); // Ensure 'FLAGS_v' visible in other threads.
    }
  }

  void revert()
  {
    if (timeout.remaining() == Seconds(0)) {
      set(original);
    }
  }

  // TODO(dhamon): Convert to static function.
  static const std::string TOGGLE_HELP;

  Timeout timeout;

  const int32_t original; // Original value of FLAGS_v.
};

} // namespace process {

#endif // __PROCESS_LOGGING_HPP__
