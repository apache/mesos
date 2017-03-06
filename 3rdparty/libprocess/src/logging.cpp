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

#include "process/delay.hpp"
#include "process/future.hpp"
#include "process/help.hpp"
#include "process/http.hpp"
#include "process/logging.hpp"

#include "stout/numify.hpp"
#include "stout/option.hpp"
#include "stout/stringify.hpp"
#include "stout/try.hpp"

namespace process {

Future<http::Response> Logging::toggle(
    const http::Request& request,
    const Option<http::authentication::Principal>&)
{
  Option<std::string> level = request.url.query.get("level");
  Option<std::string> duration = request.url.query.get("duration");

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

  return set_level(v.get(), d.get())
      .then([]() -> http::Response {
        return http::OK();
      });
}


Future<Nothing> Logging::set_level(int level, const Duration& duration)
{
  // Set the logging level.
  set(level);

  // Start a revert timer (if necessary).
  if (level != original) {
    timeout = duration;
    delay(timeout.remaining(), this, &This::revert);
  }

  return Nothing();
}


const std::string Logging::TOGGLE_HELP()
{
  return HELP(
    TLDR(
        "Sets the logging verbosity level for a specified duration."),
    DESCRIPTION(
        "The libprocess library uses [glog][glog] for logging. The library",
        "only uses verbose logging which means nothing will be output unless",
        "the verbosity level is set (by default it's 0, libprocess uses levels"
        " 1, 2, and 3).",
        "",
        "**NOTE:** If your application uses glog this will also affect",
        "your verbose logging.",
        "",
        "Query parameters:",
        "",
        ">        level=VALUE          Verbosity level (e.g., 1, 2, 3)",
        ">        duration=VALUE       Duration to keep verbosity level",
        ">                             toggled (e.g., 10secs, 15mins, etc.)"),
    AUTHENTICATION(true),
    None(),
    REFERENCES(
        "[glog]: https://code.google.com/p/google-glog"));
}

} // namespace process {
