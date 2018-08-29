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

#ifndef __STOUT_OS_LSOF_HPP__
#define __STOUT_OS_LSOF_HPP__


// For readability, we minimize the number of #ifdef blocks in the code by
// splitting platform specific system calls into separate directories.
#ifdef __WINDOWS__
#include <stout/os/windows/lsof.hpp>
#else
#include <stout/os/posix/lsof.hpp>
#endif // __WINDOWS__


#endif // __STOUT_OS_LSOF_HPP__
