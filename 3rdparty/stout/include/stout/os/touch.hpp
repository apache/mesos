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

#ifndef __STOUT_OS_TOUCH_HPP__
#define __STOUT_OS_TOUCH_HPP__

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#include <stout/os/close.hpp>
#include <stout/os/exists.hpp>
#include <stout/os/int_fd.hpp>
#include <stout/os/open.hpp>
#include <stout/os/utime.hpp>

#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__


namespace os {

inline Try<Nothing> touch(const std::string& path)
{
  if (!os::exists(path)) {
    Try<int_fd> fd = os::open(
        path,
        O_RDWR | O_CREAT,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if (fd.isError()) {
      return Error("Failed to open file: " + fd.error());
    }

    return os::close(fd.get());
  }

  // Update the access and modification times.
  return os::utime(path);
}

} // namespace os {


#endif // __STOUT_OS_TOUCH_HPP__
