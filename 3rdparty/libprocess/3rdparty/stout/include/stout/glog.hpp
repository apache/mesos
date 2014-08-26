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

#ifndef __STOUT_GLOG_HPP__
#define __STOUT_GLOG_HPP__

#include <signal.h> // For sigaction(), sigemptyset().
#include <string.h> // For strsignal().

#include <glog/logging.h>
#include <glog/raw_logging.h>

#include <string>

#include <glog/logging.h> // Includes LOG(*), PLOG(*), CHECK, etc.

// NOTE: We use RAW_LOG instead of LOG because RAW_LOG doesn't
// allocate any memory or grab locks. And according to
// https://code.google.com/p/google-glog/issues/detail?id=161
// it should work in 'most' cases in signal handlers.
namespace internal {

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

} // namespace internal {


// Installs failure handlers for signals (SIGSEGV, SIGILL, SIGFPE, SIGABRT
// and SIGBUS) to print stack traces.
// NOTE: SIGPIPE is escalated to SIGABRT to get a stack trace.
inline void installFailureSignalHandler()
{
  // Handles SIGSEGV, SIGILL, SIGFPE, SIGABRT, SIGBUS, SIGTERM
  // by default.
  google::InstallFailureSignalHandler();

  // Set up our custom signal handlers.
  struct sigaction action;
  action.sa_sigaction = internal::handler;

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

#endif // __STOUT_GLOG_HPP__
