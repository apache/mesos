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

#ifndef __STATUS_UTILS_HPP__
#define __STATUS_UTILS_HPP__

#include <string>

#include <stout/option.hpp>
#include <stout/stringify.hpp>

// Return whether the wait(2) status was a successful process exit.
inline bool WSUCCEEDED(int status)
{
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}


inline std::string WSTRINGIFY(int status)
{
#ifdef __WINDOWS__
  // On Windows the exit codes are not standardized. The behaviour should
  // be defined to improve the diagnostic based on logs.
  // TODO(dpravat): MESOS-5417 tracks this improvement.
  LOG(WARNING) << "`WSTRINGIFY` has been called, but it is not implemented.";

  return "";
#else
  std::string message;
  if (WIFEXITED(status)) {
    message += "exited with status ";
    message += stringify(WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    message += "terminated with signal ";
    message += strsignal(WTERMSIG(status));
    if (WCOREDUMP(status)) {
      message += " (core dumped)";
    }
  } else if (WIFSTOPPED(status)) {
    message += "stopped on signal ";
    message += strsignal(WSTOPSIG(status));
  } else {
    message += "wait status ";
    message += stringify(status);
  }
  return message;
#endif // __WINDOWS__
}

#endif // __STATUS_UTILS_HPP__
