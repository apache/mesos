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

#ifndef __STOUT_UNIMPLEMENTED_HPP__
#define __STOUT_UNIMPLEMENTED_HPP__

// If the project is configured to use static assertions for
// unimplemented functions.
#ifdef ENABLE_STATIC_UNIMPLEMENTED
#define UNIMPLEMENTED static_assert(false, "Unimplemented function");
#else
// Otherwise we display an error message at runtime and abort.

#include <stdlib.h>

#include <iostream>

#include <stout/attributes.hpp>

#define UNIMPLEMENTED Unimplemented(__func__, __FILE__, __LINE__)

NORETURN inline void Unimplemented(
    const char* function,
    const char* file,
    const int line)
{
  std::cerr << "Reached unimplemented function '" << function << "' at "
            << file << ':' << line << std::endl;
  abort();
}
#endif

#endif // __STOUT_UNIMPLEMENTED_HPP__
