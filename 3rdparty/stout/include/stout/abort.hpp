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

#ifndef __STOUT_ABORT_HPP__
#define __STOUT_ABORT_HPP__

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __WINDOWS__
#include <stout/windows.hpp>
#else
#include <unistd.h>
#endif // __WINDOWS__

#include <string>

#include <stout/attributes.hpp>

// NOTE: These macros are already defined in Visual Studio (Windows) headers.
#ifndef __WINDOWS__
#define __STRINGIZE(x) #x
#define _STRINGIZE(x) __STRINGIZE(x)
#endif // __WINDOWS__

// Signal safe abort which prints a message.
#define _ABORT_PREFIX "ABORT: (" __FILE__ ":" _STRINGIZE(__LINE__) "): "

#define ABORT(...) _Abort(_ABORT_PREFIX, __VA_ARGS__)


inline NORETURN void _Abort(const char* prefix, const char* message)
{
  // NOTE: On Windows, `_write` takes an `unsigned int`, not `size_t`. We
  // preform an explicit type conversion here to silence the warning. `strlen`
  // always returns a positive result, which means it is safe to cast it to an
  // unsigned value.
#ifndef __WINDOWS__
  const size_t prefix_len = strlen(prefix);
  const size_t message_len = strlen(message);
#else
  const unsigned int prefix_len = static_cast<unsigned int>(strlen(prefix));
  const unsigned int message_len = static_cast<unsigned int>(strlen(message));
#endif // !__WINDOWS__

  // Write the failure message in an async-signal safe manner,
  // assuming strlen is async-signal safe or optimized out.
  // In fact, it is highly unlikely that strlen would be
  // implemented in an unsafe manner:
  // http://austingroupbugs.net/view.php?id=692
  while (::write(STDERR_FILENO, prefix, prefix_len) == -1 &&
         errno == EINTR);
  while (message != nullptr &&
         ::write(STDERR_FILENO, message, message_len) == -1 &&
         errno == EINTR);

  // NOTE: Since `1` can be interpreted as either an `unsigned int` or a
  // `size_t`, removing the `static_cast` here makes this call ambiguous
  // between the `write` in windows.hpp and the (deprecated) `write` in the
  // Windows CRT headers.
  while (::write(STDERR_FILENO, "\n", static_cast<size_t>(1)) == -1 &&
         errno == EINTR);
  abort();
}


inline NORETURN void _Abort(const char* prefix, const std::string& message)
{
  _Abort(prefix, message.c_str());
}


#endif // __STOUT_ABORT_HPP__
