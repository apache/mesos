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

#include <signal.h> // For sigaction(), sigemptyset().
#include <string.h> // For strsignal().

#include <glog/logging.h>
#include <glog/raw_logging.h>

#include <string>

#include <process/once.hpp>

#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "logging/logging.hpp"

using process::Once;

using std::string;

namespace mesos {
namespace internal {
namespace logging {

// Persistent copy of argv0 since InitGoogleLogging requires the
// string we pass to it to be accessible indefinitely.
string argv0;


// NOTE: We use RAW_LOG instead of LOG because RAW_LOG doesn't
// allocate any memory or grab locks. And according to
// https://code.google.com/p/google-glog/issues/detail?id=161
// it should work in 'most' cases in signal handlers.
void handler(int signal)
{
  if (signal == SIGTERM) {
    RAW_LOG(WARNING, "Received signal SIGTERM; exiting.");

    // Setup the default handler for SIGTERM so that we don't print
    // a stack trace.
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_DFL;
    sigaction(signal, &action, NULL);
    raise(signal);
  } else if (signal == SIGPIPE) {
    RAW_LOG(WARNING, "Received signal SIGPIPE; escalating to SIGABRT");
    raise(SIGABRT);
  } else {
    RAW_LOG(FATAL, "Unexpected signal in signal handler: %d", signal);
  }
}


void initialize(
    const string& _argv0,
    const Flags& flags,
    bool installFailureSignalHandler)
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
    // Do not log to stderr instead of log files.
    FLAGS_logtostderr = false;
  } else {
    // Log to stderr instead of log files.
    FLAGS_logtostderr = true;
  }

  // Log everything to stderr IN ADDITION to log files unless
  // otherwise specified.
  if (flags.quiet) {
    FLAGS_stderrthreshold = 3; // FATAL.

    // FLAGS_stderrthreshold is ignored when logging to stderr instead of log files.
    // Setting the minimum log level gets around this issue.
    if (FLAGS_logtostderr) {
      FLAGS_minloglevel = 3; // FATAL.
    }
  } else {
    FLAGS_stderrthreshold = 0; // INFO.
  }

  FLAGS_logbufsecs = flags.logbufsecs;

  google::InitGoogleLogging(argv0.c_str());

  VLOG(1) << "Logging to " <<
    (flags.log_dir.isSome() ? flags.log_dir.get() : "STDERR");

  if (installFailureSignalHandler) {
    // Handles SIGSEGV, SIGILL, SIGFPE, SIGABRT, SIGBUS, SIGTERM
    // by default.
    google::InstallFailureSignalHandler();

    // Set up our custom signal handlers.
    struct sigaction action;
    action.sa_handler = handler;

    // Do not block additional signals while in the handler.
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    // Set up the SIGPIPE signal handler to escalate to SIGABRT
    // in order to have the glog handler catch it and print all
    // of its lovely information.
    if (sigaction(SIGPIPE, &action, NULL) < 0) {
      PLOG(FATAL) << "Failed to set sigaction";
    }

    // We also do not want SIGTERM to dump a stacktrace, as this
    // can imply that we crashed, when we were in fact terminated
    // by user request.
    if (sigaction(SIGTERM, &action, NULL) < 0) {
      PLOG(FATAL) << "Failed to set sigaction";
    }
  }

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
