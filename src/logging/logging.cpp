/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <glog/logging.h>

#include <map>
#include <string>
#include <vector>

#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/once.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "logging/logging.hpp"

using namespace process;
using namespace process::http;

using std::map;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace logging {

class LoggingProcess : public Process<LoggingProcess>
{
public:
  LoggingProcess()
    : ProcessBase("logging"),
      original(FLAGS_v)
  {
    // Make sure all reads/writes can be done atomically (i.e., to
    // make sure VLOG(*) statements don't read partial writes).
    // TODO(benh): Use "atomics" primitives for doing reads/writes of
    // FLAGS_v anyway to account for proper memory barriers.
    CHECK(sizeof(FLAGS_v) == sizeof(int32_t));
  }

  virtual ~LoggingProcess() {}

protected:
  virtual void initialize()
  {
    route("/toggle", &This::toggle);
  }

private:
  Future<Response> toggle(const Request& request)
  {
    Option<string> level = request.query.get("level");
    Option<string> duration = request.query.get("duration");

    if (level.isNone() && duration.isNone()) {
      return OK(stringify(FLAGS_v) + "\n");
    }

    if (level.isSome() && duration.isNone()) {
      return BadRequest("Expecting 'duration=value' in query.\n");
    } else if (level.isNone() && duration.isSome()) {
      return BadRequest("Expecting 'level=value' in query.\n");
    }

    Try<int> v = numify<int>(level.get());

    if (v.isError()) {
      return BadRequest(v.error() + ".\n");
    }

    if (v.get() < 0) {
      return BadRequest("Invalid level '" + stringify(v.get()) + "'.\n");
    } else if (v.get() < original) {
      return BadRequest("'" + stringify(v.get()) + "' < original level.\n");
    }

    Try<Duration> d = Duration::parse(duration.get());

    if (d.isError()) {
      return BadRequest(d.error() + ".\n");
    }

    // Set the logging level.
    set(v.get());

    // Start a revert timer (if necessary).
    if (v.get() != original) {
      timeout = d.get();
      delay(timeout.remaining(), this, &This::revert);
    }

    return OK();
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

  Timeout timeout;

  const int32_t original; // Original value of FLAGS_v.
};


// Persistent copy of argv0 since InitGoogleLogging requires the
// string we pass to it to be accessible indefinitely.
string argv0;


void initialize(const string& _argv0, const Flags& flags)
{
  static Once* initialized = new Once();

  if (initialized->once()) {
    return;
  }

  argv0 = _argv0;

  // Set glog's parameters through Google Flags variables.
  if (flags.log_dir.isSome()) {
    Try<Nothing> mkdir = os::mkdir(flags.log_dir.get());
    if (mkdir.isError()) {
      EXIT(1) << "Could not initialize logging: Failed to create directory "
              << flags.log_dir.get() << ": " << mkdir.error();
    }
    FLAGS_log_dir = flags.log_dir.get();
  }

  // Log everything to stderr IN ADDITION to log files unless
  // otherwise specified.
  if (!flags.quiet) {
    FLAGS_stderrthreshold = 0; // INFO.
  }

  FLAGS_logbufsecs = flags.logbufsecs;

  google::InitGoogleLogging(argv0.c_str());

  VLOG(1) << "Logging to " <<
    (flags.log_dir.isSome() ? flags.log_dir.get() : "STDERR");

  // TODO(benh): Make sure this always succeeds and never actually
  // exits (i.e., use a supervisor which re-spawns appropriately).
  spawn(new LoggingProcess(), true);

  initialized->done();
}


Try<string> getLogFile(google::LogSeverity severity)
{
  if (FLAGS_log_dir.empty()) {
    return Error("The 'log_dir' option was not specified");
  }

  Try<string> basename = os::basename(argv0);
  if (basename.isError()) {
    return Error(basename.error());
  }

  if (severity < 0 || google::NUM_SEVERITIES <= severity) {
    return Error("Unknown log severity: " + stringify(severity));
  }

  string suffix(google::GetLogSeverityName(severity));

  return path::join(FLAGS_log_dir, basename.get()) + "." + suffix;
}

} // namespace logging {
} // namespace internal {
} // namespace mesos {
