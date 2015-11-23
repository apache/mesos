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

#ifndef __STOUT_THREAD_LOCAL_HPP__
#define __STOUT_THREAD_LOCAL_HPP__

// A wrapper around the thread local storage attribute. The default
// clang on OSX does not support the c++11 standard `thread_local`
// intentionally until a higher performance implementation is
// released. See https://devforums.apple.com/message/1079348#1079348
// Until then, we use `__thread` on OSX instead.
// We required that THREAD_LOCAL is only used with POD types as this
// is a requirement of `__thread`.
#ifdef __APPLE__
#define THREAD_LOCAL __thread
#else
#define THREAD_LOCAL thread_local
#endif

#endif // __STOUT_THREAD_LOCAL_HPP__
