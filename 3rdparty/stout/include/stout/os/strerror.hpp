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

#ifndef __STOUT_OS_STRERROR_HPP__
#define __STOUT_OS_STRERROR_HPP__

#include <string.h>

#include <string>

#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__

namespace os {

/**
 * A thread-safe version of strerror.
 */
inline std::string strerror(int errno_)
{
  // There are two versions of strerror_r that need to be handled
  // based on the feature test macros below.
#if !defined(__GLIBC__) || \
    ((_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600) && \
     !defined(_GNU_SOURCE))
  // (1) We have the XSI-compliant version which returns an error code.
  size_t size = 1024;
  char* buffer = new char[size];

  while (true) {
    if (::strerror_r(errno_, buffer, size) == ERANGE) {
      delete[] buffer;
      size *= 2;
      buffer = new char[size];
    } else {
      const std::string message = buffer;
      delete[] buffer;
      return message;
    }
  }
#else
  // (2) We have the GNU-specific version which returns a char*.
  // For known error codes, strerror_r will not use the caller
  // provided buffer and will instead return a pointer to
  // internal storage. The buffer provided by the caller is only
  // only used for unknown error messages. So, we ensure that
  // the buffer can hold "Unknown error <20-digit 8-byte number>".
  // Note that the buffer will include a truncated message if the
  // buffer is not long enough.
  char buffer[1024];
  return ::strerror_r(errno_, buffer, 1024);
#endif
}

} // namespace os {
#endif // __STOUT_OS_STRERROR_HPP__
