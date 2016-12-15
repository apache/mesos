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

#ifndef __STOUT_HASHMAP_HPP__
#define __STOUT_HASHMAP_HPP__

#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <utility>

#include "foreach.hpp"
#include "hashset.hpp"
#include "none.hpp"
#include "option.hpp"


// Provides a hash map via 'std::unordered_map'. We inherit from it to add
// new functions as well as to provide better names for some of the
// existing functions.

template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
class hashmap : public std::unordered_map<Key, Value, Hash, Equal>
{
public:
  // An explicit default constructor is needed so
  // 'const hashmap<T> map;' is not an error.
  hashmap() {}

  // An implicit constructor for converting from a std::map.
  //
  // TODO(benh): Allow any arbitrary type that supports 'begin()' and
  // 'end()' passed into the specified 'emplace'?
  hashmap(const std::map<Key, Value>& map)
  {
    std::unordered_map<Key, Value, Hash, Equal>::reserve(map.size());

    for (auto iterator = map.begin(); iterator != map.end(); ++iterator) {
      std::unordered_map<Key, Value, Hash, Equal>::emplace(
          iterator->first,
          iterator->second);
    }
  }

  // An implicit constructor for converting from an r-value std::map.
  //
  // TODO(benh): Allow any arbitrary type that supports 'begin()' and
  // 'end()' passed into the specified 'insert'?
  hashmap(std::map<Key, Value>&& map)
  {
    // NOTE: We're using 'insert' here with a move iterator in order
    // to avoid copies because we know we have an r-value paramater.
    std::unordered_map<Key, Value, Hash, Equal>::insert(
        std::make_move_iterator(map.begin()),
        std::make_move_iterator(map.end()));
  }

  // Allow simple construction via initializer list.
  hashmap(std::initializer_list<std::pair<Key, Value>> list)
  {
    std::unordered_map<Key, Value, Hash, Equal>::reserve(list.size());

    for (auto iterator = list.begin(); iterator != list.end(); ++iterator) {
      std::unordered_map<Key, Value, Hash, Equal>::emplace(
          iterator->first,
          iterator->second);
    }
  }

  // Checks whether this map contains a binding for a key.
  bool contains(const Key& key) const
  {
    return std::unordered_map<Key, Value, Hash, Equal>::count(key) > 0;
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
    std::unordered_map<Key, Value, Hash, Equal>::erase(key);
    std::unordered_map<Key, Value, Hash, Equal>::insert(
        std::pair<Key, Value>(key, value));
  }

  // Returns an Option for the binding to the key.
  Option<Value> get(const Key& key) const
  {
    auto it = std::unordered_map<Key, Value, Hash, Equal>::find(key);
    if (it == std::unordered_map<Key, Value, Hash, Equal>::end()) {
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
};

#endif // __STOUT_HASHMAP_HPP__
