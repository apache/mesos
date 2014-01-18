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
#ifndef __STOUT_NUMIFY_HPP__
#define __STOUT_NUMIFY_HPP__

#include <string>

#include <boost/lexical_cast.hpp>

#include "error.hpp"
#include "none.hpp"
#include "option.hpp"
#include "result.hpp"
#include "try.hpp"

template <typename T>
Try<T> numify(const std::string& s)
{
  try {
    return boost::lexical_cast<T>(s);
  } catch (const boost::bad_lexical_cast&) {
    return Error("Failed to convert '" + s + "' to number");
  }
}


template <typename T>
Try<T> numify(const char* s)
{
  return numify<T>(std::string(s));
}


template <typename T>
Result<T> numify(const Option<std::string>& s)
{
  if (s.isSome()) {
    Try<T> t = numify<T>(s.get());
    if (t.isSome()) {
      return t.get();
    } else if (t.isError()) {
      return Error(t.error());
    }
  }

  return None();
}

#endif // __STOUT_NUMIFY_HPP__
