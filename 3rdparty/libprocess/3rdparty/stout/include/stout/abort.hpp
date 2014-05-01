/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_ABORT_HPP__
#define __STOUT_ABORT_HPP__

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Signal safe abort which prints a message.
#define __STRINGIZE(x) #x
#define _STRINGIZE(x) __STRINGIZE(x)
#define _ABORT_PREFIX "ABORT: (" __FILE__ ":" _STRINGIZE(__LINE__) "): "

#define ABORT(...) _Abort(_ABORT_PREFIX)(__VA_ARGS__)

struct _Abort
{
  _Abort(const char* _prefix) : prefix(_prefix) {}

  void operator () (
      const char* arg0 = NULL,
      const char* arg1 = NULL,
      const char* arg2 = NULL,
      const char* arg3 = NULL,
      const char* arg4 = NULL,
      const char* arg5 = NULL,
      const char* arg6 = NULL,
      const char* arg7 = NULL,
      const char* arg8 = NULL,
      const char* arg9 = NULL)
  {
    // Write the failure message in an async-signal safe manner,
    // assuming strlen is async-signal safe or optimized out.
    // In fact, it is highly unlikely that strlen would be
    // implemented in an unsafe manner:
    // http://austingroupbugs.net/view.php?id=692
    while (write(STDERR_FILENO, prefix, strlen(prefix)) == -1 &&
           errno == EINTR);
    while (arg0 != NULL &&
           write(STDERR_FILENO, arg0, strlen(arg0)) == -1 &&
           errno == EINTR);
    while (arg1 != NULL &&
           write(STDERR_FILENO, arg1, strlen(arg1)) == -1 &&
           errno == EINTR);
    while (arg2 != NULL &&
           write(STDERR_FILENO, arg2, strlen(arg2)) == -1 &&
           errno == EINTR);
    while (arg3 != NULL &&
           write(STDERR_FILENO, arg3, strlen(arg3)) == -1 &&
           errno == EINTR);
    while (arg4 != NULL &&
           write(STDERR_FILENO, arg4, strlen(arg4)) == -1 &&
           errno == EINTR);
    while (arg5 != NULL &&
           write(STDERR_FILENO, arg5, strlen(arg5)) == -1 &&
           errno == EINTR);
    while (arg6 != NULL &&
           write(STDERR_FILENO, arg6, strlen(arg6)) == -1 &&
           errno == EINTR);
    while (arg7 != NULL &&
           write(STDERR_FILENO, arg7, strlen(arg7)) == -1 &&
           errno == EINTR);
    while (arg8 != NULL &&
           write(STDERR_FILENO, arg8, strlen(arg8)) == -1 &&
           errno == EINTR);
    while (arg9 != NULL &&
           write(STDERR_FILENO, arg9, strlen(arg9)) == -1 &&
           errno == EINTR);
    abort();
  }

  const char* prefix;
};

#endif // __STOUT_ABORT_HPP__
