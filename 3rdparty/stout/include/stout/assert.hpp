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

#ifndef __STOUT_ASSERT_HPP__
#define __STOUT_ASSERT_HPP__

#include <stout/abort.hpp>

// Provide an async signal safe version `assert`. Please use `assert`
// instead if you don't need async signal safety.
#ifdef NDEBUG
#define ASSERT(e) ((void) 0)
#else
#define ASSERT(e) ((void) ((e) ? ((void) 0) : ABORT(#e)))
#endif

#endif // __STOUT_ASSERT_HPP__
