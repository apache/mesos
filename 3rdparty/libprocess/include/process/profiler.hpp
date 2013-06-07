#ifndef __PROCESS_PROFILER_HPP__
#define __PROCESS_PROFILER_HPP__

#include <glog/logging.h>

#ifdef HAS_GPERFTOOLS
#include <gperftools/profiler.h>
#endif

#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/format.hpp>
#include <stout/os.hpp>

namespace process {

const std::string PROFILE_FILE = "perftools.out";

class Profiler : public Process<Profiler>
{
public:
  Profiler() : ProcessBase("profiler"), started(false) {}

  virtual ~Profiler() {}

protected:
  virtual void initialize()
  {
    route("/start", START_HELP, &Profiler::start);
    route("/stop", STOP_HELP, &Profiler::stop);
  }

private:
  static const std::string START_HELP;
  static const std::string STOP_HELP;

  // HTTP endpoints.

  // Starts the profiler. There are no request parameters.
  Future<http::Response> start(const http::Request& request)
  {
#ifdef HAS_GPERFTOOLS
    if (os::getenv("LIBPROCESS_ENABLE_PROFILER", false) != "1") {
      return http::BadRequest(
          "The profiler is not enabled. To enable the profiler, libprocess "
          "must be started with LIBPROCESS_ENABLE_PROFILER=1 in the "
          "environment.\n");
    }

    if (started) {
      return http::BadRequest("Profiler already started.\n");
    }

    LOG(INFO) << "Starting Profiler";

    // WARNING: If using libunwind < 1.0.1, profiling should not be used, as
    // there are reports of crashes.
    // WARNING: If using libunwind 1.0.1, profiling should not be turned on
    // when it's possible for new threads to be created.
    // This may cause a deadlock. The workaround used in libprocess is described
    // here:
    // https://groups.google.com/d/topic/google-perftools/Df10Uy4Djrg/discussion
    // NOTE: We have not tested this with libunwind > 1.0.1.
    if (!ProfilerStart(PROFILE_FILE.c_str())) {
      std::string error =
        strings::format("Failed to start profiler: %s", strerror(errno)).get();
      LOG(ERROR) << error;
      return http::InternalServerError(error);
    }

    started = true;
    return http::OK("Profiler started.\n");
#else
    return http::BadRequest(
        "Perftools is disabled. To enable perftools, "
        "configure libprocess with --enable-perftools.\n");
#endif
  }

  // Stops the profiler. There are no request parameters.
  // This returns the profile output, it will also remain present
  // in the working directory.
  Future<http::Response> stop(const http::Request& request)
  {
#ifdef HAS_GPERFTOOLS
    if (!started) {
      return http::BadRequest("Profiler not running.\n");
    }

    LOG(INFO) << "Stopping Profiler";

    ProfilerStop();

    http::OK response;
    response.type = response.PATH;
    response.path = "perftools.out";
    response.headers["Content-Type"] = "application/octet-stream";
    response.headers["Content-Disposition"] =
      strings::format("attachment; filename=%s", PROFILE_FILE).get();

    started = false;
    return response;
#else
    return http::BadRequest(
        "Perftools is disabled. To enable perftools, "
        "configure libprocess with --enable-perftools.\n");
#endif
  }

  bool started;
};

} // namespace process {

#endif // __PROCESS_PROCESS_HPP__
