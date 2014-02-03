#ifndef __PROCESS_SYSTEM_HPP__
#define __PROCESS_SYSTEM_HPP__

#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/os.hpp>

namespace process {

// The System process provides HTTP endpoints for retrieving system metrics,
// such as CPU load and memory usage. This is started by default during the
// initialization of libprocess.
class System : public Process<System>
{
public:
  System() : ProcessBase("system") {}

  virtual ~System() {}

protected:
  virtual void initialize()
  {
    route("/stats.json", STATS_HELP, &System::stats);
  }

private:
  static const std::string STATS_HELP;

  // HTTP endpoints.
  Future<http::Response> stats(const http::Request& request)
  {
    JSON::Object object;
    Try<os::Load> load = os::loadavg();
    if (load.isSome()) {
      object.values["avg_load_1min"] = load.get().one;
      object.values["avg_load_5min"] = load.get().five;
      object.values["avg_load_15min"] = load.get().fifteen;
    }

    Try<long> cpus = os::cpus();
    if (cpus.isSome()) {
      object.values["cpus_total"] = cpus.get();
    }

    return http::OK(object, request.query.get("jsonp"));
  }
};


const std::string System::STATS_HELP = HELP(
    TLDR(
        "Shows local system metrics."),
    USAGE(
        "/system/stats.json"),
    DESCRIPTION(
        ">        cpus_total           Total number of available CPUs",
        ">        avg_load_1min        Average system load for last"
        " minute in uptime(1) style",
        ">        avg_load_5min        Average system load for last"
        " 5 minutes in uptime(1) style",
        ">        avg_load_15min       Average system load for last"
        " 15 minutes in uptime(1) style"));

} // namespace process {

#endif // __PROCESS_SYSTEM_HPP__
