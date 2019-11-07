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

#ifndef __STOUT_ATTRIBUTES_HPP__
#define __STOUT_ATTRIBUTES_HPP__

#ifdef __has_cpp_attribute
#  define STOUT_HAS_CPP_ATTRIBUTE(x) __has_cpp_attribute(x)
#else
#  define STOUT_HAS_CPP_ATTRIBUTE(x) 0
#endif

// We unconditionally require compiler support for `noreturn`,
// both for legacy reasons and because the presence of this
// attribute might affect code generation.
#if STOUT_HAS_CPP_ATTRIBUTE(noreturn) > 0
#  define STOUT_NORETURN [[noreturn]]
#elif defined(__WINDOWS__)
#  define STOUT_NORETURN __declspec(noreturn)
#else
#  define STOUT_NORETURN __attribute__((noreturn))
#endif

// Non-namespaced version for backwards compatibility.
#define NORETURN STOUT_NORETURN

#if STOUT_HAS_CPP_ATTRIBUTE(nodiscard) > 0
#  define STOUT_NODISCARD [[nodiscard]]
#else
#  define STOUT_NODISCARD
#endif

#endif // __STOUT_ATTRIBUTES_HPP__
