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

#ifndef __STOUT_HASHSET_HPP__
#define __STOUT_HASHSET_HPP__

#include <set>
#include <unordered_set>
#include <utility>

#include <boost/get_pointer.hpp>

#include "foreach.hpp"


// Provides a hash set via 'std::unordered_set'. We inherit from it to add
// new functions as well as to provide better naming for some of the
// existing functions.

template <typename Elem,
          typename Hash = std::hash<Elem>,
          typename Equal = std::equal_to<Elem>>
class hashset : public std::unordered_set<Elem, Hash, Equal>
{
public:
  static const hashset<Elem, Hash, Equal>& EMPTY;

  // An explicit default constructor is needed so
  // 'const hashset<T> map;' is not an error.
  hashset() {}

  // An implicit constructor for converting from a std::set.
  //
  // TODO(arojas): Allow any arbitrary type that supports 'begin()'
  // and 'end()' passed into the specified 'emplace'?
  hashset(const std::set<Elem>& set)
  {
    std::unordered_set<Elem, Hash, Equal>::reserve(set.size());

    for (auto iterator = set.begin(); iterator != set.end(); ++iterator) {
      std::unordered_set<Elem, Hash, Equal>::emplace(*iterator);
    }
  }

  // An implicit constructor for converting from an r-value std::set.
  //
  // TODO(arojas): Allow any arbitrary type that supports 'begin()'
  // and 'end()' passed into the specified 'insert'?
  hashset(std::set<Elem>&& set)
  {
    // An implementation based on the move constructor of 'hashmap'
    // fails to compile on all major compilers except gcc 5.1 and up.
    // See http://stackoverflow.com/q/31051466/118750?sem=2.
    std::unordered_set<Elem, Hash, Equal>::reserve(set.size());

    for (auto iterator = set.begin(); iterator != set.end(); ++iterator) {
      std::unordered_set<Elem, Hash, Equal>::emplace(std::move(*iterator));
    }
  }

  // Allow simple construction via initializer list.
  hashset(std::initializer_list<Elem> list)
  {
    std::unordered_set<Elem, Hash, Equal>::reserve(list.size());

    for (auto iterator = list.begin(); iterator != list.end(); ++iterator) {
      std::unordered_set<Elem, Hash, Equal>::emplace(*iterator);
    }
  }

  // Checks whether this map contains a binding for a key.
  bool contains(const Elem& elem) const
  {
    return std::unordered_set<Elem, Hash, Equal>::count(elem) > 0;
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
template <typename Elem, typename Hash, typename Equal>
const hashset<Elem, Hash, Equal>& hashset<Elem, Hash, Equal>::EMPTY =
  *new hashset<Elem, Hash, Equal>();


// Union operator.
template <typename Elem, typename Hash, typename Equal>
hashset<Elem, Hash, Equal> operator|(
    const hashset<Elem, Hash, Equal>& left,
    const hashset<Elem, Hash, Equal>& right)
{
  // Note, we're not using 'set_union' since it affords us no benefit
  // in efficiency and is more complicated to use given we have sets.
  hashset<Elem, Hash, Equal> result = left;
  result.insert(right.begin(), right.end());
  return result;
}

#endif // __STOUT_HASHSET_HPP__
