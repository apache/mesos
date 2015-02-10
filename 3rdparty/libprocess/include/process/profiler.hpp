#ifndef __PROCESS_PROFILER_HPP__
#define __PROCESS_PROFILER_HPP__

#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

namespace process {

class Profiler : public Process<Profiler>
{
public:
  Profiler() : ProcessBase("profiler"), started(false) {}

  virtual ~Profiler() {}

protected:
  virtual void initialize()
  {
    route("/start", START_HELP(), &Profiler::start);
    route("/stop", STOP_HELP(), &Profiler::stop);
  }

private:
  static const std::string START_HELP();
  static const std::string STOP_HELP();

  // HTTP endpoints.

  // Starts the profiler. There are no request parameters.
  Future<http::Response> start(const http::Request& request);

  // Stops the profiler. There are no request parameters.
  // This returns the profile output, it will also remain present
  // in the working directory.
  Future<http::Response> stop(const http::Request& request);

  bool started;
};

} // namespace process {

#endif // __PROCESS_PROCESS_HPP__
