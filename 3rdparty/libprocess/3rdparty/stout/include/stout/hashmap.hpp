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
#ifndef __STOUT_HASHMAP_HPP__
#define __STOUT_HASHMAP_HPP__

#include <utility>

#include <boost/get_pointer.hpp>
#include <boost/unordered_map.hpp>

#include "hashset.hpp"
#include "foreach.hpp"
#include "none.hpp"
#include "option.hpp"


// Provides a hash map via Boost's 'unordered_map'. For most intensive
// purposes this could be accomplished with a templated typedef, but
// those don't exist (until C++-11). Also, doing it this way allows us
// to add functionality, or better naming of existing functionality,
// etc.

template <typename Key, typename Value>
class hashmap : public boost::unordered_map<Key, Value>
{
public:
  // An explicit default constructor is needed so
  // 'const hashmap<T> map;' is not an error.
  hashmap() {}

  // Checks whether this map contains a binding for a key.
  bool contains(const Key& key) const
  {
    return boost::unordered_map<Key, Value>::count(key) > 0;
  }

  // Checks whether there exists a bound value in this map.
  bool containsValue(const Value& v) const
  {
    foreachvalue (const Value& value, *this) {
      if (value == v) {
        return true;
      }
    }
    return false;
  }

  // Inserts a key, value pair into the map replacing an old value
  // if the key is already present.
  void put(const Key& key, const Value& value)
  {
    boost::unordered_map<Key, Value>::erase(key);
    boost::unordered_map<Key, Value>::insert(std::pair<Key, Value>(key, value));
  }

  // Returns an Option for the binding to the key.
  Option<Value> get(const Key& key) const
  {
    typedef typename boost::unordered_map<Key, Value>::const_iterator
        const_iterator;
    const_iterator it = boost::unordered_map<Key, Value>::find(key);
    if (it == boost::unordered_map<Key, Value>::end()) {
      return None();
    }
    return it->second;
  }

  // Returns the set of keys in this map.
  // TODO(vinod/bmahler): Should return a list instead.
  hashset<Key> keys() const
  {
    hashset<Key> result;
    foreachkey (const Key& key, *this) {
      result.insert(key);
    }
    return result;
  }

  // Returns the list of values in this map.
  std::list<Value> values() const
  {
    std::list<Value> result;
    foreachvalue (const Value& value, *this) {
      result.push_back(value);
    }
    return result;
  }

  // Checks whether there exists a value in this map that returns the
  // a result equal to 'r' when the specified method is invoked.
  template <typename R, typename T>
  bool existsValue(R (T::*method)(), R r) const
  {
    foreachvalue (const Value& value, *this) {
      const T* t = boost::get_pointer(value);
      if (t->*method() == r) {
        return true;
      }
    }
  }

  // Checks whether there exists a value in this map whose specified
  // member is equal to 'r'.
  template <typename R, typename T>
  bool existsValue(R (T::*member), R r) const
  {
    foreachvalue (const Value& value, *this) {
      const T* t = boost::get_pointer(value);
      if (t->*member == r) {
        return true;
      }
    }
  }
};

#endif // __STOUT_HASHMAP_HPP__
