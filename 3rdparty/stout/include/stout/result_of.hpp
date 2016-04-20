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

#ifndef __STOUT_RESULT_OF_HPP__
#define __STOUT_RESULT_OF_HPP__

#include <type_traits>

#ifndef __WINDOWS__
using std::result_of;
#else
// TODO(mpark): Switch back to simply using `std::result_of` this once we
// upgrade our Windows support to VS 2015 Update 2 (MESOS-3993).

#include <utility>

namespace internal {

// TODO(mpark): Consider pulling this out to something like <stout/meta.hpp>,
// This pattern already exists in `<process/future.hpp>`.
struct LessPrefer {};
struct Prefer : LessPrefer {};


// A tag type that indicates substitution failure.
struct Fail;

// Perform the necessary expression SFINAE in a context supported in VS 2015
// Update 1. Note that it leverages `std::invoke` which is carefully written to
// avoid the limitations around the partial expression SFINAE support.

// `std::invoke` is a C++17 feature, but we only compile this code for Windows,
// which has it implemented in VS 2015 Update 1. It is also supposed to be
// defined in `<functional>`, but is included in `<utility>` in VS.
template <typename F, typename... Args>
auto result_of_test(Prefer)
  -> decltype(std::invoke(std::declval<F>(), std::declval<Args>()...));


// Report `Fail` if expression SFINAE fails in the above overload.
template <typename, typename...>
Fail result_of_test(LessPrefer);


// The generic case where `std::invoke(f, args...)` is well-formed.
template <typename T>
struct result_of_impl { using type = T; };


// The specialization for SFINAE failure case where
// `std::invoke(f, args...)` is ill-formed.
template <>
struct result_of_impl<Fail> {};


template <typename F>
struct result_of;  // undefined.


// `decltype(result_of_test<F, Args...>(Prefer()))` is either `Fail` in the case
// of substitution failure, or the return type of `std::invoke(f, args...)`.
// `result_of_impl` provides a member typedef `type = T` only if `T != Fail`.
template <typename F, typename... Args>
struct result_of<F(Args...)>
  : result_of_impl<decltype(result_of_test<F, Args...>(Prefer()))> {};

} // namespace internal {

using internal::result_of;

#endif // __WINDOWS__

#endif // __STOUT_RESULT_OF_HPP__
