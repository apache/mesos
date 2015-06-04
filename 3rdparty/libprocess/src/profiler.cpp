#include <string>

#include <glog/logging.h>

#ifdef HAS_GPERFTOOLS
#include <gperftools/profiler.h>
#endif

#include "process/future.hpp"
#include "process/help.hpp"
#include "process/http.hpp"
#include "process/profiler.hpp"

#include "stout/format.hpp"
#include "stout/option.hpp"
#include "stout/os.hpp"

namespace process {

namespace {

const char PROFILE_FILE[] = "perftools.out";

}  // namespace {

const std::string Profiler::START_HELP()
{
  return HELP(
    TLDR(
        "Starts profiling ..."),
    USAGE(
        "/profiler/start..."),
    DESCRIPTION(
        "...",
        "",
        "Query parameters:",
        "",
        ">        param=VALUE          Some description here"));
}


const std::string Profiler::STOP_HELP()
{
  return HELP(
    TLDR(
        "Stops profiling ..."),
    USAGE(
        "/profiler/stop..."),
    DESCRIPTION(
        "...",
        "",
        "Query parameters:",
        "",
        ">        param=VALUE          Some description here"));
}


Future<http::Response> Profiler::start(const http::Request& request)
{
#ifdef HAS_GPERFTOOLS
  const Option<std::string>
    enableProfiler = os::getenv("LIBPROCESS_ENABLE_PROFILER");
  if (enableProfiler.isNone() || enableProfiler.get() != "1") {
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


Future<http::Response> Profiler::stop(const http::Request& request)
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

} // namespace process {
