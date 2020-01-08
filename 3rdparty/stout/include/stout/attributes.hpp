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


// We need to special-case clang because some older versions of
// clang claim that they support `[[deprecated]]`, but will emit
// a warning that it should only be used after C++14 (failing the
// build if warnings are treated as errors).
//
// TODO(bevers): Add an optional argument to this macro, so users
// can specify a deprecation reason and a recommended alternative.
#if defined(__clang__) && __cplusplus >= 201402L
#  define STOUT_DEPRECATED [[deprecated]]
#elif defined(__clang__)
#  define STOUT_DEPRECATED __attribute__((deprecated))
#elif STOUT_HAS_CPP_ATTRIBUTE(deprecated) > 0
#  define STOUT_DEPRECATED [[deprecated]]
#else
#  define STOUT_DEPRECATED
#endif


#endif // __STOUT_ATTRIBUTES_HPP__
