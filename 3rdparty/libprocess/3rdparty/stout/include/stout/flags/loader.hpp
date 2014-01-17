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
#ifndef __STOUT_FLAGS_LOADER_HPP__
#define __STOUT_FLAGS_LOADER_HPP__

#include <string>

#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/some.hpp>
#include <stout/try.hpp>

#include <stout/flags/parse.hpp>

namespace flags {

// Forward declaration.
class FlagsBase;

template <typename T>
struct Loader
{
  static Try<Nothing> load(
      T* flag,
      const lambda::function<Try<T>(const std::string&)>& parse,
      const std::string& name,
      const std::string& value)
  {
    Try<T> t = parse(value);
    if (t.isSome()) {
      *flag = t.get();
    } else {
      return Error("Failed to load value '" + value + "': " + t.error());
    }
    return Nothing();
  }
};


template <typename T>
struct OptionLoader
{
  static Try<Nothing> load(
      Option<T>* flag,
      const lambda::function<Try<T>(const std::string&)>& parse,
      const std::string& name,
      const std::string& value)
  {
    Try<T> t = parse(value);
    if (t.isSome()) {
      *flag = Some(t.get());
    } else {
      return Error("Failed to load value '" + value + "': " + t.error());
    }
    return Nothing();
  }
};


template <typename F, typename T>
struct MemberLoader
{
  static Try<Nothing> load(
      FlagsBase* base,
      T F::*flag,
      const lambda::function<Try<T>(const std::string&)>& parse,
      const std::string& name,
      const std::string& value)
  {
    F* f = dynamic_cast<F*>(base);
    if (f != NULL) {
      Try<T> t = parse(value);
      if (t.isSome()) {
        f->*flag = t.get();
      } else {
        return Error("Failed to load value '" + value + "': " + t.error());
      }
    }
    return Nothing();
  }
};


template <typename F, typename T>
struct OptionMemberLoader
{
  static Try<Nothing> load(
      FlagsBase* base,
      Option<T> F::*flag,
      const lambda::function<Try<T>(const std::string&)>& parse,
      const std::string& name,
      const std::string& value)
  {
    F* f = dynamic_cast<F*>(base);
    if (f != NULL) {
      Try<T> t = parse(value);
      if (t.isSome()) {
        f->*flag = Some(t.get());
      } else {
        return Error("Failed to load value '" + value + "': " + t.error());
      }
    }
    return Nothing();
  }
};

} // namespace flags {

#endif // __STOUT_FLAGS_LOADER_HPP__
