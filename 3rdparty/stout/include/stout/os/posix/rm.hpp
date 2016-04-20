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
#ifndef __STOUT_OS_POSIX_RM_HPP__
#define __STOUT_OS_POSIX_RM_HPP__

#include <stdio.h>

#include <string>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>


namespace os {

inline Try<Nothing> rm(const std::string& path)
{
  if (::remove(path.c_str()) != 0) {
    return ErrnoError();
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_POSIX_RM_HPP__
