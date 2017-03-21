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

#ifndef __STOUT_OS_POSIX_TEMP_HPP__
#define __STOUT_OS_POSIX_TEMP_HPP__

#include <string>

#include <stout/os/getenv.hpp>


namespace os {

// Attempts to resolve the system-designated temporary directory before
// back on a sensible default. On POSIX platforms, this involves checking
// the POSIX-standard `TMPDIR` environment variable before falling
// back to `/tmp`.
inline std::string temp()
{
  Option<std::string> tmpdir = os::getenv("TMPDIR");

  return tmpdir.getOrElse("/tmp");
}

} // namespace os {

#endif // __STOUT_OS_POSIX_TEMP_HPP__
