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

#ifndef __STOUT_OVERLOAD_HPP__
#define __STOUT_OVERLOAD_HPP__

#include <stout/traits.hpp>


// Using `overload` you can pass in callable objects that have
// `operator()` and get a new callable object that has all of the
// `operator()`s pulled in. For example:
//
//   auto lambdas = overload(
//       [](int i) { return stringify(i); },
//       [](double d) { return stringify(d); },
//       [](const std::string& s) { return s; });
//
// See stout/variant.hpp for how this is used to visit variants.
//
// NOTE: `overload` is declared and defined below `Overload` because
// and we can't declare `overload` here and define it below because it
// uses an `auto` return type.

template <typename F, typename... Fs>
struct Overload;


template <typename F>
struct Overload<F> : F
{
  using F::operator();

  // NOTE: while not strictly necessary, we include `result_type` so
  // that this can be used places where `result_type` is required,
  // e.g., `boost::apply_visitor`.
  using result_type = typename LambdaTraits<F>::result_type;

  template <typename G>
  Overload(G&& g) : F(std::forward<G>(g)) {}
};


template <typename F, typename... Fs>
struct Overload : F, Overload<Fs...>
{
  using F::operator();
  using Overload<Fs...>::operator();

  // NOTE: while not strictly necessary, we include `result_type` so
  // that this can be used places where `result_type` is required,
  // e.g., `boost::apply_visitor`.
  using result_type = typename LambdaTraits<F>::result_type;

  template <typename G, typename... Gs>
  Overload(G&& g, Gs&&... gs)
    : F(std::forward<G>(g)), Overload<Fs...>(std::forward<Gs>(gs)...) {}
};


template <typename... Fs>
auto overload(Fs&&... fs)
  -> decltype(Overload<Fs...>(std::forward<Fs>(fs)...))
{
  return Overload<Fs...>(std::forward<Fs>(fs)...);
}

#endif // __STOUT_OVERLOAD_HPP__
