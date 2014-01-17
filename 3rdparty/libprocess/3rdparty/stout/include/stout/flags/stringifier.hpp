/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_FLAGS_STRINGIFIER_HPP__
#define __STOUT_FLAGS_STRINGIFIER_HPP__

#include <string>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <stout/flags/parse.hpp>

namespace flags {

// Forward declaration.
class FlagsBase;

template<typename T>
static Option<std::string> Stringifier(T* value)
{
  return stringify(*value);
}


template<typename T>
static Option<std::string> OptionStringifier(Option<T>* value)
{
  if (value->isSome()) {
    return stringify(value->get());
  }
  return None();
}


template<typename F, typename T>
static Option<std::string> MemberStringifier(
    const FlagsBase& base,
    T F::*flag)
{
  const F* f = dynamic_cast<const F*>(&base);
  if (f != NULL) {
    return stringify(f->*flag);
  }
  return None();
}


template<typename F, typename T>
static Option<std::string> OptionMemberStringifier(
    const FlagsBase& base,
    Option<T> F::*flag)
{
  const F* f = dynamic_cast<const F*>(&base);
  if (f != NULL) {
    const Option<T>& v = f->*flag;
    if (v.isSome()) {
      return stringify(v.get());
    }
  }
  return None();
}

} // namespace flags {

#endif // __STOUT_FLAGS_STRINGIFIER_HPP__
