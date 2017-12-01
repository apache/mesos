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

#ifndef __STOUT_CPP14_HPP__
#define __STOUT_CPP14_HPP__

#include <cstddef>

// This file contains implementation of C++14 standard library features.
// Once we adopt C++14, this file should be removed and usages of its
// functionality should be replaced with the standard library equivalents
// by replacing `cpp14` with `std` and including the appropriate headers.

// Note that code in this file may not follow stout conventions strictly
// as it uses names as defined in C++ standard.

namespace cpp14 {

// This is a simplified implementation of C++14 `std::index_sequence`
// and `std::make_index_sequence` from <utility>.
template <typename T, T... Is>
struct integer_sequence
{
  static constexpr std::size_t size() noexcept { return sizeof...(Is); }
};


namespace internal {

template <typename T, std::size_t N, std::size_t... Is>
struct IntegerSequenceGen : IntegerSequenceGen<T, N - 1, N - 1, Is...> {};


template <typename T, std::size_t... Is>
struct IntegerSequenceGen<T, 0, Is...>
{
  using type = integer_sequence<T, Is...>;
};

} // namespace internal {


template <typename T, std::size_t N>
using make_integer_sequence = typename internal::IntegerSequenceGen<T, N>::type;


template <std::size_t... Is>
using index_sequence = integer_sequence<std::size_t, Is...>;


template <std::size_t N>
using make_index_sequence = make_integer_sequence<std::size_t, N>;

} // namespace cpp14 {

#endif // __STOUT_CPP14_HPP__
