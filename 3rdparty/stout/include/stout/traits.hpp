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

#ifndef __STOUT_TRAITS_HPP__
#define __STOUT_TRAITS_HPP__

template <template <typename...> class T, typename U>
struct is_specialization_of : std::false_type {};

template <template <typename...> class T, typename... Args>
struct is_specialization_of<T, T<Args...>> : std::true_type {};


// Lambda (or functor) traits.
template <typename T>
struct LambdaTraits : public LambdaTraits<decltype(&T::operator())> {};


template <typename Class, typename Result, typename... Args>
struct LambdaTraits<Result(Class::*)(Args...) const>
{
  typedef Result result_type;
};


// Helper for checking if a type `U` is the same as or convertible to
// _at least one of_ the types `Ts`.
//
// Example usage:
//
//   std::enable_if<AtLeastOneIsSameOrConvertible<U, Ts>::value>
template <typename...>
struct AtLeastOneIsSameOrConvertible
{
  static constexpr bool value = false;
};


template <typename U, typename T, typename... Ts>
struct AtLeastOneIsSameOrConvertible<U, T, Ts...>
{
  static constexpr bool value =
    std::is_same<U, T>::value ||
    std::is_convertible<U, T>::value ||
    AtLeastOneIsSameOrConvertible<U, Ts...>::value;
};

#endif // __STOUT_TRAITS_HPP__
