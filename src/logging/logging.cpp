// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <signal.h> // For sigaction(), sigemptyset().
#include <string.h> // For strsignal().

#ifdef __WINDOWS__
#include <windows.h>
#include <dbghelp.h>
#endif

#include <glog/logging.h>
#include <glog/raw_logging.h>

#ifdef __WINDOWS__
#include <atomic>
#include <cstdio>
#endif
#include <string>

#include <process/once.hpp>

#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/signals.hpp>

#include "logging/logging.hpp"

#ifdef __linux__
// Declare FLAGS_drop_log_memory flag for glog. This declaration is based on the
// the DECLARE_XXX macros from glog/logging.h.
namespace fLB {
  extern GOOGLE_GLOG_DLL_DECL bool FLAGS_drop_log_memory;
}
using fLB::FLAGS_drop_log_memory;
#endif

using process::Once;

using std::cerr;
using std::endl;
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
  UNREACHABLE();
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
#ifndef __WINDOWS__
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
  } else {
    RAW_LOG(FATAL, "Unexpected signal in signal handler: %d", signal);
  }
}
#endif // __WINDOWS__


#ifdef __WINDOWS__
// We use a lock to protect dbghelp.h methods, since they are not
// thread safe and need to be synchronized.
std::atomic_flag dbghelpLock = ATOMIC_FLAG_INIT;

// Fills in the stack trace from the provided exception context.
// NOTE: Do not allocate memory or do anything else that might
// obtain a lock acquired by code running outside of the exception
// filter, since that lock may still be held since the stack is
// unwound. (See POSIX async-signal-safety to better understand
// the importance of this).
//
// NOTE: Rather than returning the number of filled in entries,
// this returns the number of frames in the stack trace, which may be
// larger than the provided trace array (in which case, the caller
// needs to be careful not to index past the end!). This enables
// the caller to detect that there were more frames that this
// function was not able to fill in.
//
// This code is adapted from Chromium's implementation:
//
// https://github.com/chromium/chromium/blob/3771ffb16f922cf9ddc40b733498cc88f82f8fcf/base/debug/stack_trace_win.cc#L295-L339 // NOLINT
size_t stackTraceFromExceptionContext(
    const CONTEXT* context, void** trace, size_t size)
{
  size_t traceCount = 0;

  // StackWalk64 modifies the context in place; we supply it with a copy
  // so that downstream exception handlers get the right context.
  CONTEXT contextCopy;
  memcpy(&contextCopy, context, sizeof(contextCopy));
  contextCopy.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;

  STACKFRAME64 frame;
  memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
  int machineType = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrPC.Offset = context->Rip;
  frame.AddrFrame.Offset = context->Rbp;
  frame.AddrStack.Offset = context->Rsp;
#elif defined(__aarch64__)
  int machineType = IMAGE_FILE_MACHINE_ARM64;
  frame.AddrPC.Offset = context->Pc;
  frame.AddrFrame.Offset = context->Fp;
  frame.AddrStack.Offset = context->Sp;
#elif defined(_M_IX86) || defined(__i386__)
  int machineType = IMAGE_FILE_MACHINE_I386;
  frame.AddrPC.Offset = context->Eip;
  frame.AddrFrame.Offset = context->Ebp;
  frame.AddrStack.Offset = context->Esp;
#else
#error Unsupported Windows Architecture
#endif
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Mode = AddrModeFlat;

  // Acquire the lock to protect StackWalk64.
  while (dbghelpLock.test_and_set(std::memory_order_acquire));

  while (StackWalk64(machineType, GetCurrentProcess(), GetCurrentThread(),
                     &frame, &contextCopy, NULL,
                     &SymFunctionTableAccess64, &SymGetModuleBase64, NULL)) {
    if (traceCount < size) {
      trace[traceCount] = reinterpret_cast<void*>(frame.AddrPC.Offset);
    }
    ++traceCount;
  }

  // Release the lock protecting StackWalk64.
  dbghelpLock.clear(std::memory_order_release);

  for (size_t i = traceCount; i < size; ++i) {
    trace[i] = NULL;
  }

  return traceCount;
}


// This code is inspired from the Chromium and Glog projects, as well
// as the MSDN documentation and examples:
//
// https://github.com/chromium/chromium/blob/3771ffb16f922cf9ddc40b733498cc88f82f8fcf/base/debug/stack_trace_win.cc#L197-L256 // NOLINT
// https://github.com/google/glog/blob/130a3e10de248344cdaeda54aed4c8a5ad7cedac/src/symbolize.cc#L908-L933 // NOLINT
// https://docs.microsoft.com/en-us/windows/win32/debug/retrieving-symbol-information-by-address // NOLINT
void Symbolize(void* pc, char* out, size_t size)
{
  char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];

  DWORD64 symbolDisplacement = 0;
  SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = MAX_SYM_NAME - 1;

  // Acquire the lock to protect SymFromAddr and SymGetLineFromAddr64.
  while (dbghelpLock.test_and_set(std::memory_order_acquire));

  BOOL symbolFound = SymFromAddr(
      GetCurrentProcess(),
      reinterpret_cast<DWORD64>(pc),
      &symbolDisplacement,
      symbol);

  DWORD lineDisplacement = 0;
  IMAGEHLP_LINE64 line = {};
  line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

  BOOL lineFound = SymGetLineFromAddr64(
      GetCurrentProcess(),
      reinterpret_cast<DWORD64>(pc),
      &lineDisplacement,
      &line);

  // Release the lock protecting SymFromAddr and SymGetLineFromAddr64.
  dbghelpLock.clear(std::memory_order_release);

  // We handle each case individually here since it winds up being
  // simpler than dealing with offsets into the output buffer and
  // error handling, and it's easier to see what the output looks like.
  if (symbolFound) {
    if (lineFound) {
      snprintf(out, size, "\t%s [%p+%llu] (%s:%u)",
               symbol->Name,
               pc, symbolDisplacement,
               line.FileName, line.LineNumber);
    } else {
      snprintf(out, size, "\t%s [%p+%llu]",
               symbol->Name,
               pc, symbolDisplacement);
    }
  } else {
    if (lineFound) {
      snprintf(out, size, "\t(No symbol) [%p+%llu] (%s:%u)",
               pc, symbolDisplacement,
               line.FileName, line.LineNumber);
    } else {
      snprintf(out, size, "\t(No symbol) [%p+%llu]",
               pc, symbolDisplacement);
    }
  }
}


long WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
  // At this point the stack has already been unwound so we have to walk
  // the exception's stack information from the provided `info` (i.e. we
  // can't just RAW_LOG(FATAL) like we do on POSIX.
  //
  // Use raw logging since the stack has been unwound and locks may be
  // held (see `handler(...)` above). Log as error even though this is
  // fatal since we don't want to crash here.
  //
  // It appears there is no simple way to convert the error code into
  // a string, so we hard code the names.

  DWORD code = info->ExceptionRecord->ExceptionCode;

  RAW_LOG(ERROR, "Received fatal exception %#x %s", code, [code]() {
    switch (code) {
      case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
      case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
      case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
      case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
      case EXCEPTION_FLT_DENORMAL_OPERAND:
        return "EXCEPTION_FLT_DENORMAL_OPERAND";
      case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return  "EXCEPTION_FLT_DIVIDE_BY_ZERO";
      case EXCEPTION_FLT_INEXACT_RESULT:
        return "EXCEPTION_FLT_INEXACT_RESULT";
      case EXCEPTION_FLT_INVALID_OPERATION:
        return "EXCEPTION_FLT_INVALID_OPERATION";
      case EXCEPTION_FLT_OVERFLOW:
        return "EXCEPTION_FLT_OVERFLOW";
      case EXCEPTION_FLT_STACK_CHECK:
        return "EXCEPTION_FLT_STACK_CHECK";
      case EXCEPTION_FLT_UNDERFLOW:
        return "EXCEPTION_FLT_UNDERFLOW";
      case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
      case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
      case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
      case EXCEPTION_INT_OVERFLOW:
        return "EXCEPTION_INT_OVERFLOW";
      case EXCEPTION_INVALID_DISPOSITION:
        return "EXCEPTION_INVALID_DISPOSITION";
      case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
      case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION";
      case EXCEPTION_SINGLE_STEP:
        return "EXCEPTION_SINGLE_STEP";
      case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
      default:
        return "UNKNOWN";
  }}());

  // Acquire the lock to protect SymSetOptions and SymInitialize.
  while (dbghelpLock.test_and_set(std::memory_order_acquire));

  SymSetOptions(SYMOPT_DEFERRED_LOADS |
                SYMOPT_UNDNAME |
                SYMOPT_LOAD_LINES);

  BOOL initialized = SymInitialize(GetCurrentProcess(), NULL, TRUE);

  // Release the lock protecting SymSetOptions and SymInitialize.
  dbghelpLock.clear(std::memory_order_release);

  if (!initialized) {
    DWORD error = GetLastError();
    char buffer[64];
    strerror_s(buffer, sizeof(buffer), error);
    RAW_LOG(ERROR, "Unable to print exception stack trace:"
                   " Failed to SymInitialize: %s", buffer);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const size_t MAX_TRACE_SIZE = 250;

  void* trace[MAX_TRACE_SIZE];

  size_t traceCount = stackTraceFromExceptionContext(
      info->ContextRecord, trace, MAX_TRACE_SIZE);

  for (size_t i = 0; i < std::min(traceCount, MAX_TRACE_SIZE); ++i) {
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR) + 16];

    Symbolize(trace[i], buffer, sizeof(buffer));

    RAW_LOG(ERROR, "%s", buffer);
  }

  if (traceCount > MAX_TRACE_SIZE) {
    RAW_LOG(ERROR, "\t< %u additional frames clipped >",
            traceCount - MAX_TRACE_SIZE);
  }

  return EXCEPTION_CONTINUE_SEARCH;
}
#endif


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
    bool installFailureSignalHandler,
    const Option<Flags>& _flags)
{
  static Once* initialized = new Once();

  if (initialized->once()) {
    return;
  }

  argv0 = _argv0;

  // Use the default flags if not specified.
  Flags flags;
  if (_flags.isSome()) {
    flags = _flags.get();

    FLAGS_minloglevel = getLogSeverity(flags.logging_level);
    FLAGS_logbufsecs = flags.logbufsecs;
  }

  if (flags.logging_level != "INFO" &&
      flags.logging_level != "WARNING" &&
      flags.logging_level != "ERROR") {
    cerr << "'" << flags.logging_level << "' is not a valid logging level."
         << " Possible values for 'logging_level' flag are:"
         << " 'INFO', 'WARNING', 'ERROR'." << endl;
    exit(EXIT_FAILURE);
  }

  if (flags.log_dir.isSome()) {
    Try<Nothing> mkdir = os::mkdir(flags.log_dir.get());
    if (mkdir.isError()) {
      cerr << "Could not initialize logging: Failed to create directory "
           << flags.log_dir.get() << ": " << mkdir.error() << endl;
      exit(EXIT_FAILURE);
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

#ifdef __linux__
  // Do not drop in-memory buffers of log contents. When set to true, this flag
  // can significantly slow down the master. The slow down is attributed to
  // several hundred `posix_fadvise(..., POSIX_FADV_DONTNEED)` calls per second
  // to advise the kernel to drop in-memory buffers related to log contents.
  // We set this flag to 'false' only if the corresponding environment variable
  // is not set.
  if (os::getenv("GLOG_drop_log_memory").isNone()) {
    FLAGS_drop_log_memory = false;
  }
#endif

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
#ifdef __WINDOWS__
    // Glog on windows now supports InstallFailureSignalHandler(),
    // but it is of little utility since windows handles failures
    // via structured exception handling (SEH) instead. While some
    // signals may get mapped by the c runtime library (CRT) [1], when
    // we tested this out it worked for SIGABRT but it did not work
    // for SIGSEGV. So, we just use an unhandled exception filter to
    // catch these exceptions and dump a stack trace.
    //
    // [1] https://docs.oracle.com/en/java/javase/13/troubleshoot/handle-signals-and-exceptions.html#GUID-43732853-4FDD-4FED-99A0-56B79B44B3AD // NOLINT
    //
    // See the following issues for having glog provide this:
    //
    //   https://github.com/google/glog/issues/535
    //   https://github.com/google/glog/issues/534
    //
    // TODO(bmahler): Does it make sense to use AddVectoredContinueHandler
    // (and push to back of list), this appears to execute after vectored
    // exception handlers and SEH, which seems like the last chance to
    // look at the exception.
    SetUnhandledExceptionFilter(&UnhandledExceptionFilter);
#else
    // Handles SIGSEGV, SIGILL, SIGFPE, SIGABRT, SIGBUS, SIGTERM
    // by default.
    google::InstallFailureSignalHandler();

    // Set up our custom signal handlers.
    //
    // NOTE: The code below sets the SIGTERM signal handler to the `handle`
    // function declared above. While this is useful on POSIX systems, SIGTERM
    // is generated and handled differently on Windows[1], so this code would
    // not work.
    //
    // [1] https://msdn.microsoft.com/en-us/library/xdkz3x12.aspx
    struct sigaction action;
    action.sa_sigaction = handler;

    // Do not block additional signals while in the handler.
    sigemptyset(&action.sa_mask);

    // The SA_SIGINFO flag tells sigaction() to use
    // the sa_sigaction field, not sa_handler.
    action.sa_flags = SA_SIGINFO;

    // We also do not want SIGTERM to dump a stacktrace, as this
    // can imply that we crashed, when we were in fact terminated
    // by user request.
    if (sigaction(SIGTERM, &action, nullptr) < 0) {
      PLOG(FATAL) << "Failed to set sigaction";
    }
#endif // __WINDOWS__
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
