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

#include <stout/os/signals.hpp>

#include "logging/logging.hpp"

// Declare FLAGS_drop_log_memory flag for glog. This declaration is based on the
// the DECLARE_XXX macros from glog/logging.h.
namespace fLB {
  extern GOOGLE_GLOG_DLL_DECL bool FLAGS_drop_log_memory;
}
using fLB::FLAGS_drop_log_memory;

using process::Once;

using std::string;

// Captures the stack trace and exits when a pure virtual method is
// called.
// This interface is part of C++ ABI.
//   -Linux c++abi:
//    http://refspecs.linuxbase.org/cxxabi-1.83.html#vcall
//      "This routine will only be called if the user calls a
//       non-overridden pure virtual function, which has undefined
//       behavior according to the C++ Standard".
//   -OSX libc++abi:
//    http://libcxxabi.llvm.org/spec.html
// This usage has been tested on Linux & OSX and on gcc & clang.
extern "C" void __cxa_pure_virtual()
{
  RAW_LOG(FATAL, "Pure virtual method called");
}


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
inline void handler(int signal, siginfo_t *siginfo, void *context)
{
  if (signal == SIGTERM) {
    if (siginfo->si_code == SI_USER ||
        siginfo->si_code == SI_QUEUE ||
        siginfo->si_code <= 0) {
      RAW_LOG(WARNING, "Received signal SIGTERM from process %d of user %d; "
                       "exiting", siginfo->si_pid, siginfo->si_uid);
    } else {
      RAW_LOG(WARNING, "Received signal SIGTERM; exiting");
    }

    // Setup the default handler for SIGTERM so that we don't print
    // a stack trace.
    os::signals::reset(signal);
    raise(signal);
  } else if (signal == SIGPIPE) {
    RAW_LOG(WARNING, "Received signal SIGPIPE; escalating to SIGABRT");
    raise(SIGABRT);
  } else {
    RAW_LOG(FATAL, "Unexpected signal in signal handler: %d", signal);
  }
}


google::LogSeverity getLogSeverity(const string& logging_level)
{
  if (logging_level == "INFO") {
    return google::INFO;
  } else if (logging_level == "WARNING") {
    return google::WARNING;
  } else if (logging_level == "ERROR") {
    return google::ERROR;
  } else {
    // TODO(bmahler): Consider an error here.
    return google::INFO;
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

  if (flags.logging_level != "INFO" &&
      flags.logging_level != "WARNING" &&
      flags.logging_level != "ERROR") {
    EXIT(1) << "'" << flags.logging_level << "' is not a valid logging level."
               " Possible values for 'logging_level' flag are: "
               " 'INFO', 'WARNING', 'ERROR'.";
  }

  FLAGS_minloglevel = getLogSeverity(flags.logging_level);

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

    // FLAGS_stderrthreshold is ignored when logging to stderr instead
    // of log files. Setting the minimum log level gets around this issue.
    if (FLAGS_logtostderr) {
      FLAGS_minloglevel = 3; // FATAL.
    }
  } else {
    FLAGS_stderrthreshold = FLAGS_minloglevel;
  }

  FLAGS_logbufsecs = flags.logbufsecs;

  // Do not drop in-memory buffers of log contents. When set to true, this flag
  // can significantly slow down the master. The slow down is attributed to
  // several hundred `posix_fadvise(..., POSIX_FADV_DONTNEED)` calls per second
  // to advise the kernel to drop in-memory buffers related to log contents.
  // We set this flag to 'false' only if the corresponding environment variable
  // is not set.
  if (os::getenv("GLOG_drop_log_memory").isNone()) {
    FLAGS_drop_log_memory = false;
  }

  google::InitGoogleLogging(argv0.c_str());
  if (flags.log_dir.isSome()) {
    // Log this message in order to create the log file; this is because GLOG
    // creates the log file once the first log message occurs; also recreate
    // the file if it has been created on a previous run.
    LOG_AT_LEVEL(FLAGS_minloglevel)
      << google::GetLogSeverityName(FLAGS_minloglevel)
      << " level logging started!";
  }

  VLOG(1) << "Logging to " <<
    (flags.log_dir.isSome() ? flags.log_dir.get() : "STDERR");

  if (installFailureSignalHandler) {
    // Handles SIGSEGV, SIGILL, SIGFPE, SIGABRT, SIGBUS, SIGTERM
    // by default.
    google::InstallFailureSignalHandler();

    // Set up our custom signal handlers.
    struct sigaction action;
    action.sa_sigaction = handler;

    // Do not block additional signals while in the handler.
    sigemptyset(&action.sa_mask);

    // The SA_SIGINFO flag tells sigaction() to use
    // the sa_sigaction field, not sa_handler.
    action.sa_flags = SA_SIGINFO;

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

  if (severity < 0 || google::NUM_SEVERITIES <= severity) {
    return Error("Unknown log severity: " + stringify(severity));
  }

  return path::join(FLAGS_log_dir, Path(argv0).basename()) + "." +
         google::GetLogSeverityName(severity);
}

} // namespace logging {
} // namespace internal {
} // namespace mesos {
