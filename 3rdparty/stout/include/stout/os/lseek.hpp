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

#ifndef __STOUT_OS_LSEEK_HPP__
#define __STOUT_OS_LSEEK_HPP__

#ifdef __WINDOWS__
#include <io.h>
#else
#include <unistd.h>
#endif

#include <stout/error.hpp>
#include <stout/try.hpp>

#include <stout/os/int_fd.hpp>

namespace os {

inline Try<off_t> lseek(int_fd fd, off_t offset, int whence)
{
#ifdef __WINDOWS__
  off_t result = ::_lseek(fd.crt(), offset, whence);
#else
  off_t result = ::lseek(fd, offset, whence);
#endif
  if (result < 0) {
    return ErrnoError();
  }
  return result;
}

} // namespace os {

#endif // __STOUT_OS_LSEEK_HPP__
