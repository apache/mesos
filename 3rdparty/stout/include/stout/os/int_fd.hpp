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

#ifndef __STOUT_OS_INT_FD_HPP__
#define __STOUT_OS_INT_FD_HPP__


// For readability, we minimize the number of #ifdef blocks in the code by
// splitting platform specifc system calls into separate directories.
#ifdef __WINDOWS__
#include <stout/os/windows/fd.hpp>
#endif // __WINDOWS__

// The `int_fd` type is designed to be able to keep / continue to write the
// existing POSIX file descriptor pattern in a portable manner with Windows.
//
// IMPORTANT: Use the `int_fd` in platform-agnostic code paths, and use `int`
//            or `os::WindowsFD` directly in platform-specific code paths.
//
// NOTE: The `int_` prefix is meant to indicate that on POSIX, `int_fd` will
// behave exactly as-is.
using int_fd =
#ifdef __WINDOWS__
  os::WindowsFD;
#else
  int;
#endif // __WINDOWS__

#endif // __STOUT_OS_INT_FD_HPP__
