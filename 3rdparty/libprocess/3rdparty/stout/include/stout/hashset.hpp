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
#ifndef __STOUT_HASHSET_HPP__
#define __STOUT_HASHSET_HPP__

#include <boost/get_pointer.hpp>
#include <boost/unordered_set.hpp>

#include "foreach.hpp"


// Provides a hash set via Boost's 'unordered_set'. For most intensive
// purposes this could be accomplished with a templated typedef, but
// those don't exist (until C++-11). Also, doing it this way allows us
// to add functionality, or better naming of existing functionality,
// etc.

template <typename Elem>
class hashset : public boost::unordered_set<Elem>
{
public:
  static const hashset<Elem>& EMPTY;

  // An explicit default constructor is needed so
  // 'const hashset<T> map;' is not an error.
  hashset() {}

  // Checks whether this map contains a binding for a key.
  bool contains(const Elem& elem) const
  {
    return boost::unordered_set<Elem>::count(elem) > 0;
  }

  // Checks whether there exists a value in this set that returns the
  // a result equal to 'r' when the specified method is invoked.
  template <typename R, typename T>
  bool exists(R (T::*method)(), R r) const
  {
    foreach (const Elem& elem, *this) {
      const T* t = boost::get_pointer(elem);
      if (t->*method() == r) {
        return true;
      }
    }
  }

  // Checks whether there exists an element in this set whose
  // specified member is equal to 'r'.
  template <typename R, typename T>
  bool exists(R (T::*member), R r) const
  {
    foreach (const Elem& elem, *this) {
      const T* t = boost::get_pointer(elem);
      if (t->*member == r) {
        return true;
      }
    }
  }
};


// TODO(jmlvanre): Possibly remove this reference as per MESOS-2694.
template <typename Elem>
const hashset<Elem>& hashset<Elem>::EMPTY = *new hashset<Elem>();


// Union operator.
template <typename Elem>
hashset<Elem> operator | (const hashset<Elem>& left, const hashset<Elem>& right)
{
  // Note, we're not using 'set_union' since it affords us no benefit
  // in efficiency and is more complicated to use given we have sets.
  hashset<Elem> result = left;
  result.insert(right.begin(), right.end());
  return result;
}

#endif // __STOUT_HASHSET_HPP__
