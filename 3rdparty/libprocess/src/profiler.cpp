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

#include <string>

#include <glog/logging.h>

#ifdef ENABLE_GPERFTOOLS
#include <gperftools/profiler.h>
#endif

#include "process/future.hpp"
#include "process/help.hpp"
#include "process/http.hpp"
#include "process/profiler.hpp"

#include "stout/format.hpp"
#include "stout/option.hpp"
#include "stout/os.hpp"
#include "stout/os/strerror.hpp"

namespace process {

#ifdef ENABLE_GPERFTOOLS
namespace {
constexpr char PROFILE_FILE[] = "perftools.out";
} // namespace {
#endif

const std::string Profiler::START_HELP()
{
  return HELP(
    TLDR(
        "Start profiling."),
    DESCRIPTION(
        "Start to use google perftools do profiling."),
    AUTHENTICATION(true));
}


const std::string Profiler::STOP_HELP()
{
  return HELP(
    TLDR(
        "Stops profiling."),
    DESCRIPTION(
        "Stop to use google perftools do profiling."),
    AUTHENTICATION(true));
}


Future<http::Response> Profiler::start(
    const http::Request& request,
    const Option<http::authentication::Principal>&)
{
#ifdef ENABLE_GPERFTOOLS
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
  if (!ProfilerStart(PROFILE_FILE)) {
    Try<std::string> error =
      strings::format("Failed to start profiler: %s", os::strerror(errno));
    LOG(ERROR) << error.get();
    return http::InternalServerError(error.get());
  }

  started = true;
  return http::OK("Profiler started.\n");
#else
  return http::BadRequest(
      "Perftools is disabled. To enable perftools, "
      "configure libprocess with --enable-perftools.\n");
#endif
}


Future<http::Response> Profiler::stop(
    const http::Request& request,
    const Option<http::authentication::Principal>&)
{
#ifdef ENABLE_GPERFTOOLS
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
