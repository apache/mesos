#ifndef __PROCESS_LOGGING_HPP__
#define __PROCESS_LOGGING_HPP__

#include <glog/logging.h>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

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
    route("/toggle", TOGGLE_HELP(), &This::toggle);
  }

private:
  Future<http::Response> toggle(const http::Request& request);

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

  static const std::string TOGGLE_HELP();

  Timeout timeout;

  const int32_t original; // Original value of FLAGS_v.
};

} // namespace process {

#endif // __PROCESS_LOGGING_HPP__
