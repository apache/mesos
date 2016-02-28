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

#ifndef __STOUT_REPRESENTATION_HPP__
#define __STOUT_REPRESENTATION_HPP__

#include <functional>

// The class template `Representation` is a generic base class to support types
// that have multiple human-readable representations of itself.
//
// `Time` is a good example of such a type. Even though the __underlying__
// representation may be a `std::chrono::time_point` or even a `time_t`, there
// are multiple formats in which we can print them out.
//
// NOTE: These classes are light-weight objects that simply holds a reference to
// a type `T`, and therefore cannot be used with a temporary.
//
// NOTE: The pattern is to inherit from `Representation`, and pull in
// `Representation`'s constructors with using declarations. The explicit `using`
// declarations are necessary since inheritance does not inherit the base class'
// constructors. Also note that a type alias is not sufficient, since we need to
// give rise to distinct types.
//
// Example:
//
// ```
// // Two different representations of `Time`.
//
// struct RFC1123 : Representation<Time>
// {
//   using Representation<Time>::Representation;
// };
//
// struct RFC3339 : Representation<Time>
// {
//   using Representation<Time>::Representation;
// };
//
// // `operator<<` for the two different representations of `Time`.
//
// std::ostream& operator<<(std::ostream& stream, const RFC1123& rfc1123)
// {
//    const Time& t = rfc1123;
//    // Print in `RFC1123` format.
// }
//
// std::ostream& operator<<(std::ostream& stream, const RFC3339& rfc3339)
// {
//    const Time& t = rfc1123;
//    // Print in `RFC3339` format.
// }
//
// int main()
// {
//   Time t = ...;
//   std::cout << RFC1123(t);  // Print in `RFC1123` format.
//   std::cout << RFC3339(t);  // Print in `RFC1123` format.
// }
// ```

template <typename T>
struct Representation : std::reference_wrapper<const T>
{
  // We pull in `std::reference_wrapper`'s constructors since inheritance does
  // not inherit the base class' constructors.
  using std::reference_wrapper<const T>::reference_wrapper;

  explicit Representation(const T& t) : std::reference_wrapper<const T>(t) {}

  // Disallow rebinding.
  Representation& operator=(const Representation&) = delete;
  Representation& operator=(Representation&&) = delete;
};

#endif // __STOUT_REPRESENTATION_HPP__
