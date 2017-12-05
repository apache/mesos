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

#ifndef __STOUT_LAMBDA_HPP__
#define __STOUT_LAMBDA_HPP__

#include <algorithm>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

#include <stout/cpp14.hpp>
#include <stout/cpp17.hpp>
#include <stout/result_of.hpp>

namespace lambda {

using std::bind;
using std::cref;
using std::function;
using std::ref;

using namespace std::placeholders;


template <
  template <typename...> class Iterable,
  typename F,
  typename U,
  typename V = typename result_of<F(U)>::type,
  typename... Us>
Iterable<V> map(F&& f, const Iterable<U, Us...>& input)
{
  Iterable<V> output;
  std::transform(
      input.begin(),
      input.end(),
      std::inserter(output, output.begin()),
      std::forward<F>(f));
  return output;
}


template <
  template <typename...> class OutputIterable,
  template <typename...> class InputIterable,
  typename F,
  typename U,
  typename V = typename result_of<F(U)>::type,
  typename... Us>
OutputIterable<V> map(F&& f, const InputIterable<U, Us...>& input)
{
  OutputIterable<V> output;
  std::transform(
      input.begin(),
      input.end(),
      std::inserter(output, output.begin()),
      std::forward<F>(f));
  return output;
}


template <
  template <typename...> class Iterable,
  typename F,
  typename U,
  typename V = typename result_of<F(U)>::type,
  typename = typename std::enable_if<
    !std::is_same<U, V>::value>::type,
  typename... Us>
Iterable<V> map(F&& f, Iterable<U, Us...>&& input)
{
  Iterable<V> output;
  std::transform(
      std::make_move_iterator(input.begin()),
      std::make_move_iterator(input.end()),
      std::inserter(output, output.begin()),
      std::forward<F>(f));
  return output;
}


template <
  template <typename...> class Iterable,
  typename F,
  typename U,
  typename = typename std::enable_if<
    std::is_same<U, typename result_of<F(U)>::type>::value>::type,
  typename... Us>
Iterable<U, Us...>&& map(F&& f, Iterable<U, Us...>&& iterable)
{
  std::transform(
      std::make_move_iterator(iterable.begin()),
      std::make_move_iterator(iterable.end()),
      iterable.begin(),
      std::forward<F>(f));
  return std::move(iterable);
}


template <
  template <typename...> class OutputIterable,
  template <typename...> class InputIterable,
  typename F,
  typename U,
  typename V = typename result_of<F(U)>::type,
  typename... Us>
OutputIterable<V> map(F&& f, InputIterable<U, Us...>&& input)
{
  OutputIterable<V> output;
  std::transform(
      std::make_move_iterator(input.begin()),
      std::make_move_iterator(input.end()),
      std::inserter(output, output.begin()),
      std::forward<F>(f));
  return output;
}


template <
  template <typename...> class OutputIterable,
  typename F,
  typename U,
  typename V = typename result_of<F(U)>::type>
OutputIterable<V> map(F&& f, std::initializer_list<U> input)
{
  OutputIterable<V> output;
  std::transform(
      input.begin(),
      input.end(),
      std::inserter(output, output.begin()),
      std::forward<F>(f));
  return output;
}


template <
  typename F,
  typename U,
  typename V = typename result_of<F(U)>::type>
std::vector<V> map(F&& f, std::initializer_list<U> input)
{
  std::vector<V> output;
  std::transform(
      input.begin(),
      input.end(),
      std::inserter(output, output.begin()),
      std::forward<F>(f));
  return output;
}


#define RETURN(...) -> decltype(__VA_ARGS__) { return __VA_ARGS__; }


namespace internal {

// The `int` specializations here for `is_placeholder<T>::value`.
// `is_placeholder<T>::value` returns a `0` for non-placeholders,
// and I > 0 for placeholders where I indicates the placeholder
// value. e.g., `is_placeholder<decltype(_1)>::value == 1`

template <int I>
struct Expand
{
  // Bound argument is a placeholder.
  template <typename T, typename Args>
  auto operator()(T&&, Args&& args) const
    RETURN(std::get<I - 1>(std::forward<Args>(args)))
};


template <>
struct Expand<0>
{
  // Bound argument is not a placeholder.
  template <typename T, typename Args>
  auto operator()(T&& t, Args&&) const
    RETURN(std::forward<T>(t))
};


template <typename F, typename... BoundArgs>
class Partial
{
  F f;
  std::tuple<BoundArgs...> bound_args;

  template <typename T, typename Args>
  static auto expand(T&& t, Args&& args)
    RETURN(Expand<std::is_placeholder<typename std::decay<T>::type>::value>{}(
        std::forward<T>(t), std::forward<Args>(args)))

  // Invoke the given function `f` with bound arguments expanded. If a bound
  // argument is a placeholder, we use the index `I` of the placeholder to
  // pass the `I`th argument out of `args` along. Otherwise, we pass the bound
  // argument through preserving its value category. That is, passing the bound
  // argument as an lvalue-ref or rvalue-ref depending correspondingly on
  // whether the `Partial` itself is an lvalue or rvalue.
  template <typename F_, typename BoundArgs_, typename Args, std::size_t... Is>
  static auto invoke_expand(
      F_&& f,
      BoundArgs_&& bound_args,
      cpp14::index_sequence<Is...>,
      Args&& args)
    RETURN(cpp17::invoke(
        std::forward<F_>(f),
        expand(
            std::get<Is>(std::forward<BoundArgs_>(bound_args)),
            std::forward<Args>(args))...))

public:
  template <typename... BoundArgs_>
  explicit Partial(const F& f, BoundArgs_&&... args)
    : f(f), bound_args(std::forward<BoundArgs_>(args)...) {}

  template <typename... BoundArgs_>
  explicit Partial(F&& f, BoundArgs_&&... args)
    : f(std::move(f)), bound_args(std::forward<BoundArgs_>(args)...) {}

  Partial(const Partial&) = default;
  Partial(Partial&&) = default;

  Partial& operator=(const Partial&) = default;
  Partial& operator=(Partial&&) = default;

  template <typename... Args>
  auto operator()(Args&&... args) &
    RETURN(invoke_expand(
      f,
      bound_args,
      cpp14::make_index_sequence<sizeof...(BoundArgs)>(),
      std::forward_as_tuple(std::forward<Args>(args)...)))

  template <typename... Args>
  auto operator()(Args&&... args) const &
    RETURN(invoke_expand(
      f,
      bound_args,
      cpp14::make_index_sequence<sizeof...(BoundArgs)>(),
      std::forward_as_tuple(std::forward<Args>(args)...)))

  template <typename... Args>
  auto operator()(Args&&... args) &&
    RETURN(invoke_expand(
      std::move(f),
      std::move(bound_args),
      cpp14::make_index_sequence<sizeof...(BoundArgs)>(),
      std::forward_as_tuple(std::forward<Args>(args)...)))

  template <typename... Args>
  auto operator()(Args&&... args) const &&
    RETURN(invoke_expand(
      std::move(f),
      std::move(bound_args),
      cpp14::make_index_sequence<sizeof...(BoundArgs)>(),
      std::forward_as_tuple(std::forward<Args>(args)...)))
};

} // namespace internal {


// Performs partial function application, similar to `std::bind`. However,
// it supports moving the bound arguments through, unlike `std::bind`.
// To do so, the `operator()` must be invoked on a rvalue `lambda::partial`.
//
// Unsupported `std::bind` features:
//   - There is no special treatment for nested bind expressions. When calling
//     `operator()` on partial, call parameters will not be passed to nested
//     bind expression. Instead, bind expression will be passed as-is to the
//     wrapped function object. This behavior is intentional, for simplicity
//     reasons, and is in sync with C++20's `std::bind_front`.
//   - Passing `std::reference_wrapper` is not implemented.
template <typename F, typename... Args>
internal::Partial<
    typename std::decay<F>::type,
    typename std::decay<Args>::type...>
partial(F&& f, Args&&... args)
{
  using R = internal::Partial<
      typename std::decay<F>::type,
      typename std::decay<Args>::type...>;
  return R(std::forward<F>(f), std::forward<Args>(args)...);
}


#undef RETURN

} // namespace lambda {


namespace std {

template <typename F, typename... Args>
struct is_bind_expression<lambda::internal::Partial<F, Args...>>
  : true_type {};

} // namespace std {

#endif // __STOUT_LAMBDA_HPP__
