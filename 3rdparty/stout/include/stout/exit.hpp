// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_EXIT_HPP__
#define __STOUT_EXIT_HPP__

#include <stdlib.h>

#include <ostream>

#include <glog/logging.h>
#include <glog/raw_logging.h>

#include <stout/attributes.hpp>


// Exit takes an exit status and provides a glog stream for output
// prior to exiting. This is like glog's LOG(FATAL) or CHECK, except
// that it does _not_ print a stack trace.
//
// Ex: EXIT(EXIT_FAILURE) << "Cgroups are not present in this system.";
#define EXIT(status) __Exit(__FILE__, __LINE__, status).stream()

// Async-signal safe exit which prints a message.
//
// NOTE: We use RAW_LOG instead of LOG because RAW_LOG doesn't
// allocate any memory or grab locks. And according to
// https://code.google.com/p/google-glog/issues/detail?id=161
// it should work in 'most' cases in signal handlers.
//
// NOTE: We expect that compiler supports `,##__VA_ARGS__`, see:
// https://stackoverflow.com/questions/5588855
#define SAFE_EXIT(status, fmt, ...)                                       \
  do {                                                                    \
    if (status) {                                                         \
      RAW_LOG(ERROR, "EXIT with status %d: " fmt, status, ##__VA_ARGS__); \
    } else {                                                              \
      RAW_LOG(INFO, "EXIT with status %d: " fmt, status, ##__VA_ARGS__);  \
    }                                                                     \
    ::_exit(status);                                                      \
  } while (0)


struct __Exit
{
  __Exit(const char* file, int line, int _status)
    : status(_status),
      message(
          file,
          line,
          _status == EXIT_SUCCESS ? google::GLOG_INFO : google::GLOG_ERROR)
  {
    stream() << "EXIT with status " << _status << ": ";
  }

  STOUT_NORETURN ~__Exit()
  {
    message.Flush();
    exit(status);
  }

  std::ostream& stream()
  {
    return message.stream();
  }

  const int status;
  google::LogMessage message;
};


#endif // __STOUT_EXIT_HPP__
