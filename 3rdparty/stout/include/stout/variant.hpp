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

#ifndef __STOUT_VARIANT_HPP__
#define __STOUT_VARIANT_HPP__

#include <utility>

#include <boost/variant.hpp>

#include <stout/overload.hpp>
#include <stout/traits.hpp>
#include <stout/try.hpp>

// A wrapper of `boost::variant` so that we can add our own
// functionality (e.g., `visit` via lambdas) as well as replace the
// implementation in the future (i.e., to use `mpark::variant` or
// `std::variant`).
//
// Example usage:
//
//   Variant<int, bool> variant = "hello world";
//
//   std::string type = variant.visit(
//       [](int) { return "int"; },
//       [](bool) { return "bool"; });
//
//
//            *** IMPORANT IMPLEMENTATION DETAILS ***
//
// We do not inherit from `boost::variant` because it does not have a
// virtual destructor and therefore was not intended to be inherited
// from. This means that we need to provide our own `operator==`,
// `operator!=`, and `operator<<` for `std::ostream`. We found that
// when inheriting from `boost::variant` we needed to override
// `operator==` anyway.
template <typename T, typename... Ts>
class Variant // See above for why we don't inherit from `boost::variant`.
{
public:
  // We provide a basic constuctor that forwards to `boost::variant`.
  // Note that we needed to use SFINAE in order to keep the compiler
  // from trying to use this constructor instead of the default copy
  // constructor, move constructor, etc, as well as to keep the
  // programmer from being able to construct an instance from
  // `boost::variant` directly.
  template <typename U,
            typename Decayed = typename std::decay<U>::type,
            typename = typename std::enable_if<
              !std::is_same<Decayed, Variant>::value>::type,
            typename = typename std::enable_if<
              !std::is_same<Decayed, boost::variant<T, Ts...>>::value>::type>
  Variant(U&& u) : variant(std::forward<U>(u)) {}

  template <typename... Fs>
  auto visit(Fs&&... fs) const
    -> decltype(
        boost::apply_visitor(
            overload(std::forward<Fs>(fs)...),
            std::declval<boost::variant<T, Ts...>&>()))
  {
    return boost::apply_visitor(
        overload(std::forward<Fs>(fs)...),
        variant);
  }

  template <typename... Fs>
  auto visit(Fs&&... fs)
    -> decltype(
        boost::apply_visitor(
            overload(std::forward<Fs>(fs)...),
            std::declval<boost::variant<T, Ts...>&>()))
  {
    return boost::apply_visitor(
        overload(std::forward<Fs>(fs)...),
        variant);
  }

  // Because we don't inherit from `boost::variant` we need to provide
  // this operator ourselves, but note that even when we tried
  // inheriting from `boost::variant` we still found that we needed to
  // implement this operator in order to properly cast both operands
  // to `boost::variant`.
  bool operator==(const Variant& that) const
  {
    return variant == that.variant;
  }

  bool operator!=(const Variant& that) const
  {
    return !(*this == that);
  }

private:
  template <typename U, typename... Us>
  friend std::ostream& operator<<(std::ostream&, const Variant<U, Us...>&);

  boost::variant<T, Ts...> variant;
};


template <typename T, typename... Ts>
std::ostream& operator<<(std::ostream& stream, const Variant<T, Ts...>& variant)
{
  return stream << variant.variant;
}

#endif // __STOUT_VARIANT_HPP__
