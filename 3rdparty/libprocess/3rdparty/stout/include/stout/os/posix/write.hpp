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

#ifndef __STOUT_OS_POSIX_WRITE_HPP__
#define __STOUT_OS_POSIX_WRITE_HPP__

#include <errno.h>
#include <unistd.h>


namespace os {

// Compatibility function. On POSIX, this function is trivial, but on Windows,
// we have to check whether the file descriptor is a socket or a file to write
// to it.
using ::write;

} // namespace os {


#endif // __STOUT_OS_POSIX_READ_HPP__
